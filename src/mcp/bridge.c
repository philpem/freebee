/*
 * bridge - "freebee -mcp-bridge": the MCP server an agent starts.
 *
 * It speaks MCP on stdin and stdout, lists the tools, and passes each tool
 * call to the VM it names (by its name or PID), on that VM's private
 * socket in /tmp/freebee-<uid>/. It does vm-list and vm-wait itself, so
 * they work while no VM runs. It starts fast and never exits on its own:
 * only when the client closes stdin.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <dirent.h>
#include <unistd.h>
#include <libgen.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "mcp.h"
#include "../fbmcp.h"

static const char bridge_instructions[] =
	"- Several VMs may be running, the user's and yours. Every tool takes vm: a VM's name"
	" (its folder) or PID from vm-list. You may leave it out while only one VM runs.\n"
	"- A VM serves MCP while FreeBee runs (FREEBEE_MCP=0 turns it off). After you start"
	" one from a shell, call vm-wait instead of a sleep loop. After vm-power quit, vm-wait"
	" until gone before you copy its disks.\n";

static const char vm_prop[] =
	"\"vm\":{\"description\":\"the VM: its name (folder) or PID, from vm-list; leave it out"
	" while only one VM runs\",\"anyOf\":[{\"type\":\"string\"},{\"type\":\"integer\"}]}";

static const char vm_list_tool[] =
	"{\"name\":\"vm-list\",\"description\":\"List the running FreeBee VMs: name, PID, folder,"
	" start time, whether an agent started it, and whether it answers. Pass a name or PID as vm"
	" to the other tools.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}},"
	"\"annotations\":{\"readOnlyHint\":true,\"openWorldHint\":false}}";

static const char vm_wait_tool[] =
	"{\"name\":\"vm-wait\",\"description\":\"Wait for a VM to start or to end, instead of a sleep"
	" or pgrep loop in a shell. until up: the VM runs and its tools answer (give its name or"
	" PID; without one, any VM). With restarted, only a new process counts, so the call can"
	" begin before a restart. until gone: its FreeBee process has ended and closed its disks.\","
	"\"inputSchema\":{\"type\":\"object\",\"properties\":{"
	"\"until\":{\"type\":\"string\",\"enum\":[\"up\",\"gone\"],\"description\":\"what to wait for"
	" (default: up)\"},"
	"\"vm\":{\"description\":\"the VM's name or PID\",\"anyOf\":[{\"type\":\"string\"},"
	"{\"type\":\"integer\"}]},"
	"\"restarted\":{\"type\":\"boolean\",\"description\":\"with up: only a process that was not"
	" running when the call began (default: false)\"},"
	"\"timeout-ms\":{\"type\":\"integer\",\"description\":\"give up after this long, at most"
	" 1500000 (default: 60000)\"}}},"
	"\"annotations\":{\"readOnlyHint\":true,\"openWorldHint\":false}}";

/* ---- output ---- */

static void out_line(sbuf *m)
{
	sb_putc(m, '\n');
	size_t off = 0;
	while (off < m->len) {
		ssize_t w = write(1, m->s + off, m->len - off);
		if (w > 0)
			off += w;
		else if (w < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		else
			break;
	}
}

static void reply(const char *id, const char *result)
{
	sbuf m;
	sb_init(&m);
	sb_printf(&m, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}", id, result);
	out_line(&m);
	sb_free(&m);
}

static void reply_error(const char *id, int code, const char *text, const char *data)
{
	sbuf m;
	sb_init(&m);
	sb_printf(&m, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":", id ? id : "null", code);
	sb_jstr(&m, text);
	if (data)
		sb_printf(&m, ",\"data\":%s", data);
	sb_puts(&m, "}}");
	out_line(&m);
	sb_free(&m);
}

static void reply_text(const char *id, const char *text, bool error)
{
	sbuf r;
	sb_init(&r);
	sb_puts(&r, "{\"content\":[{\"type\":\"text\",\"text\":");
	sb_jstr(&r, text);
	sb_printf(&r, "}],\"isError\":%s}", error ? "true" : "false");
	reply(id, r.s);
	sb_free(&r);
}

static void progress(const char *token, int n, const char *msg)
{
	sbuf m;
	sb_init(&m);
	sb_printf(&m, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\","
		"\"params\":{\"progressToken\":%s,\"progress\":%d,\"message\":", token, n);
	sb_jstr(&m, msg);
	sb_puts(&m, "}}");
	out_line(&m);
	sb_free(&m);
}

/* ---- the registry ---- */

typedef struct {
	int pid;
	char name[256], dir[1024], sock[512], started[64];
	bool by_agent;
} vm_entry;

static bool alive(int pid)
{
	return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM);
}

static int connect_vm(const char *path)
{
	struct sockaddr_un sa;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof(sa.sun_path))
		return -1;
	strcpy(sa.sun_path, path);
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		close(fd);
		return -1;
	}
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	return fd;
}

static int list_vms(vm_entry *out, int max)
{
	const char *dir = mcp_registry_dir();
	if (!mcp_registry_ok(dir, false))
		return 0;
	DIR *d = opendir(dir);
	if (!d)
		return 0;
	int n = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL && n < max) {
		size_t l = strlen(e->d_name);
		if (l < 6 || strcmp(e->d_name + l - 5, ".json"))
			continue;
		char path[1024];
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		FILE *f = fopen(path, "r");
		if (!f)
			continue;
		char buf[8192];
		size_t len = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
		buf[len] = 0;
		json *j = json_parse(buf, len, NULL);
		json *pid = json_get(j, "pid");
		if (!j || !pid || pid->type != JSON_NUMBER) {
			json_free(j);
			continue;
		}
		vm_entry *v = &out[n];
		memset(v, 0, sizeof(*v));
		v->pid = (int)pid->num;
		if (!alive(v->pid)) {
			// FreeBee died without cleaning up: its files go
			unlink(path);
			char sp[1024];
			snprintf(sp, sizeof(sp), "%s/%d.sock", dir, v->pid);
			unlink(sp);
			json_free(j);
			continue;
		}
		const char *s;
		if ((s = json_get_str(j, "name"))) snprintf(v->name, sizeof(v->name), "%s", s);
		if ((s = json_get_str(j, "dir"))) snprintf(v->dir, sizeof(v->dir), "%s", s);
		if ((s = json_get_str(j, "socket"))) snprintf(v->sock, sizeof(v->sock), "%s", s);
		if ((s = json_get_str(j, "started"))) snprintf(v->started, sizeof(v->started), "%s", s);
		json *ba = json_get(j, "by-agent");
		v->by_agent = ba && ba->type == JSON_BOOL && ba->b;
		json_free(j);
		n++;
	}
	closedir(d);
	return n;
}

static bool vm_matches(const vm_entry *v, const json *sel)
{
	if (!sel || sel->type == JSON_NULL)
		return true;
	if (sel->type == JSON_NUMBER)
		return v->pid == (int)sel->num;
	if (sel->type != JSON_STRING)
		return false;
	char *end;
	long pid = strtol(sel->str, &end, 10);
	if (*sel->str && !*end)
		return v->pid == pid;
	if (strcasecmp(v->name, sel->str) == 0)
		return true;
	char tmp[1024];
	snprintf(tmp, sizeof(tmp), "%s", v->dir);
	return strcasecmp(basename(tmp), sel->str) == 0 || strcmp(v->dir, sel->str) == 0;
}

static void describe_vms(sbuf *b, vm_entry *v, int n)
{
	for (int i = 0; i < n; i++) {
		int fd = connect_vm(v[i].sock);
		if (fd >= 0)
			close(fd);
		sb_printf(b, "%s%s (pid %d) in %s, started %s%s%s", i ? "\n" : "", v[i].name, v[i].pid,
			v[i].dir, v[i].started, v[i].by_agent ? ", started by an agent" : ", the user's",
			fd >= 0 ? "" : ", NOT ANSWERING");
	}
}

/* ---- calls ---- */

typedef struct pending {
	char *id;
	char *token;
	int fd;					// the VM's socket, or -1 for vm-wait
	sbuf in;
	// vm-wait
	bool wait_gone, restarted;
	json *sel;
	int known[64], n_known;
	double started, deadline, last_progress;
	int progress;
	struct pending *next;
} pending;

static pending *calls;

static void drop(pending *p)
{
	for (pending **pp = &calls; *pp; pp = &(*pp)->next) {
		if (*pp == p) {
			*pp = p->next;
			break;
		}
	}
	if (p->fd >= 0)
		close(p->fd);
	sb_free(&p->in);
	json_free(p->sel);
	free(p->id);
	free(p->token);
	free(p);
}

static void tools_list(const char *id)
{
	static char *text;
	if (!text) {
		json *tools = json_parse(mcp_tools_json(), strlen(mcp_tools_json()), NULL);
		sbuf b;
		sb_init(&b);
		sb_puts(&b, "{\"tools\":[");
		sb_puts(&b, vm_list_tool);
		sb_putc(&b, ',');
		sb_puts(&b, vm_wait_tool);
		for (int i = 0; tools && i < tools->n; i++) {
			json *t = tools->items[i];
			sb_puts(&b, ",{");
			for (int k = 0; k < t->n; k++) {
				if (k)
					sb_putc(&b, ',');
				sb_jstr(&b, t->keys[k]);
				sb_putc(&b, ':');
				json *s = t->items[k];
				if (strcmp(t->keys[k], "inputSchema") || s->type != JSON_OBJECT) {
					json_dump(&b, s);
					continue;
				}
				// the schema, with vm added to its properties
				sb_putc(&b, '{');
				bool props = false;
				for (int m = 0; m < s->n; m++) {
					if (m)
						sb_putc(&b, ',');
					sb_jstr(&b, s->keys[m]);
					sb_putc(&b, ':');
					json *pv = s->items[m];
					if (strcmp(s->keys[m], "properties") || pv->type != JSON_OBJECT) {
						json_dump(&b, pv);
						continue;
					}
					props = true;
					sb_putc(&b, '{');
					for (int q = 0; q < pv->n; q++) {
						sb_jstr(&b, pv->keys[q]);
						sb_putc(&b, ':');
						json_dump(&b, pv->items[q]);
						sb_putc(&b, ',');
					}
					sb_puts(&b, vm_prop);
					sb_putc(&b, '}');
				}
				if (!props)
					sb_printf(&b, "%s\"properties\":{%s}", s->n ? "," : "", vm_prop);
				sb_putc(&b, '}');
			}
			sb_putc(&b, '}');
		}
		sb_puts(&b, "]}");
		json_free(tools);
		text = sb_take(&b);
	}
	reply(id, text);
}

static void vm_list(const char *id)
{
	vm_entry v[64];
	int n = list_vms(v, 64);
	sbuf b;
	sb_init(&b);
	if (!n)
		sb_puts(&b, "No FreeBee VM is running. Start one from its folder (for example ./run.sh"
			" in a shell, in the background), then call vm-wait.");
	else
		describe_vms(&b, v, n);
	reply_text(id, b.s, false);
	sb_free(&b);
}

// checks a vm-wait; true when it is answered
static bool check_wait(pending *p)
{
	vm_entry v[64];
	int n = list_vms(v, 64);
	sbuf b;
	sb_init(&b);
	bool done = false;
	if (p->wait_gone) {
		int left = 0;
		for (int i = 0; i < n; i++)
			left += vm_matches(&v[i], p->sel);
		if (!left) {
			sb_printf(&b, "Gone after %.1f s: no matching FreeBee runs.", wall_now() - p->started);
			done = true;
		}
	} else {
		for (int i = 0; i < n && !done; i++) {
			if (!vm_matches(&v[i], p->sel))
				continue;
			bool old = false;
			for (int k = 0; k < p->n_known; k++)
				old |= p->known[k] == v[i].pid;
			if (p->restarted && old)
				continue;
			int fd = connect_vm(v[i].sock);
			if (fd < 0)
				continue;
			close(fd);
			sb_printf(&b, "Up after %.1f s: ", wall_now() - p->started);
			describe_vms(&b, &v[i], 1);
			done = true;
		}
	}
	if (!done && wall_now() >= p->deadline) {
		sb_printf(&b, "Timed out after %.1f s. ", wall_now() - p->started);
		if (n)
			describe_vms(&b, v, n);
		else
			sb_puts(&b, "No FreeBee VM is running.");
		reply_text(p->id, b.s, true);
		sb_free(&b);
		return true;
	}
	if (done)
		reply_text(p->id, b.s, false);
	else if (p->token && wall_now() - p->last_progress >= 5) {
		p->last_progress = wall_now();
		progress(p->token, ++p->progress, p->wait_gone ? "waiting for the VM to end" :
			"waiting for the VM to start");
	}
	sb_free(&b);
	return done;
}

static void vm_wait(const char *id, const char *token, json *args)
{
	const char *until = json_get_str(args, "until");
	json *to = json_get(args, "timeout-ms");
	json *rs = json_get(args, "restarted");
	double timeout = to && to->type == JSON_NUMBER ? to->num / 1000.0 : 60;
	if (until && strcmp(until, "up") && strcmp(until, "gone")) {
		reply_text(id, "until must be up or gone", true);
		return;
	}
	if (timeout < 0 || timeout > WAIT_MAX_MS / 1000) {
		reply_text(id, "timeout-ms must be from 0 to 1500000", true);
		return;
	}
	pending *p = calloc(1, sizeof(*p));
	p->id = strdup(id);
	p->token = token ? strdup(token) : NULL;
	p->fd = -1;
	sb_init(&p->in);
	p->wait_gone = until && strcmp(until, "gone") == 0;
	p->restarted = rs && rs->type == JSON_BOOL && rs->b;
	json *sel = json_get(args, "vm");
	if (sel) {
		sbuf t;
		sb_init(&t);
		json_dump(&t, sel);
		p->sel = json_parse(t.s, t.len, NULL);
		sb_free(&t);
	}
	p->started = p->last_progress = wall_now();
	p->deadline = p->started + timeout;
	vm_entry v[64];
	int n = list_vms(v, 64);
	for (int i = 0; i < n && p->n_known < 64; i++)
		p->known[p->n_known++] = v[i].pid;
	if (check_wait(p)) {
		json_free(p->sel);
		free(p->id);
		free(p->token);
		sb_free(&p->in);
		free(p);
		return;
	}
	p->next = calls;
	calls = p;
}

static void forward(const char *id, const char *line, json *args)
{
	vm_entry v[64];
	int n = list_vms(v, 64);
	json *sel = json_get(args, "vm");
	int match[64], m = 0;
	for (int i = 0; i < n; i++)
		if (vm_matches(&v[i], sel))
			match[m++] = i;
	sbuf b;
	sb_init(&b);
	if (m != 1) {
		if (!n)
			sb_puts(&b, "No FreeBee VM is running. Start one (for example ./run.sh in its folder),"
				" then call vm-wait.");
		else if (!m)
			sb_puts(&b, "No running VM has that name or PID. Running: ");
		else
			sb_puts(&b, sel ? "Several VMs match; give the PID. Running: " :
				"Several VMs are running: give vm, a name or PID. Running: ");
		if (n && m != 1) {
			sb_putc(&b, '\n');
			describe_vms(&b, v, n);
		}
		reply_text(id, b.s, true);
		sb_free(&b);
		return;
	}
	vm_entry *vm = &v[match[0]];
	int fd = connect_vm(vm->sock);
	if (fd < 0) {
		sb_printf(&b, "VM %s (pid %d) does not answer on %s.", vm->name, vm->pid, vm->sock);
		reply_text(id, b.s, true);
		sb_free(&b);
		return;
	}
	sb_free(&b);
	sbuf m2;
	sb_init(&m2);
	sb_puts(&m2, line);
	sb_putc(&m2, '\n');
	size_t off = 0;
	while (off < m2.len) {
		ssize_t w = write(fd, m2.s + off, m2.len - off);
		if (w > 0)
			off += w;
		else if (w < 0 && errno == EINTR)
			continue;
		else
			break;
	}
	sb_free(&m2);
	pending *p = calloc(1, sizeof(*p));
	p->id = strdup(id);
	p->fd = fd;
	sb_init(&p->in);
	p->next = calls;
	calls = p;
}

static bool initialized;

static void client_message(const char *line, size_t len)
{
	const char *err = NULL;
	json *msg = json_parse(line, len, &err);
	if (!msg) {
		reply_error(NULL, -32700, err, NULL);
		return;
	}
	if (msg->type != JSON_OBJECT) {
		reply_error(NULL, -32600, msg->type == JSON_ARRAY ? "batches are not supported" :
			"a message must be a JSON object", NULL);
		json_free(msg);
		return;
	}
	const char *method = json_get_str(msg, "method");
	json *idv = json_get(msg, "id");
	json *params = json_get(msg, "params");
	if (!method) {
		json_free(msg);
		return;
	}
	sbuf idb;
	sb_init(&idb);
	if (idv)
		json_dump(&idb, idv);
	if (!idv) {
		if (strcmp(method, "notifications/cancelled") == 0) {
			json *rid = json_get(params, "requestId");
			sbuf r;
			sb_init(&r);
			json_dump(&r, rid);
			for (pending *p = calls; p; p = p->next) {
				if (rid && strcmp(p->id, r.s) == 0) {
					// closing the socket cancels the call in the VM
					drop(p);
					break;
				}
			}
			sb_free(&r);
		}
		goto out;
	}
	// a handshake-free client names its protocol version in _meta
	json *meta = json_get(params, "_meta");
	const char *version = NULL;
	for (int i = 0; meta && meta->type == JSON_OBJECT && i < meta->n; i++)
		if (strstr(meta->keys[i], "protocolVersion") && meta->items[i]->type == JSON_STRING)
			version = meta->items[i]->str;
	if (!initialized && version && !mcp_protocol_supported(version)) {
		sbuf d;
		sb_init(&d);
		sb_puts(&d, "{\"supported\":[\"2025-11-25\",\"2025-06-18\"],\"requested\":");
		sb_jstr(&d, version);
		sb_putc(&d, '}');
		reply_error(idb.s, -32022, "Unsupported protocol version", d.s);
		sb_free(&d);
		goto out;
	}
	if (strcmp(method, "initialize") == 0) {
		const char *asked = json_get_str(params, "protocolVersion");
		sbuf r, text;
		sb_init(&r);
		sb_init(&text);
		sb_puts(&text, mcp_instructions());
		sb_puts(&text, bridge_instructions);
		sb_puts(&r, "{\"protocolVersion\":");
		sb_jstr(&r, asked && mcp_protocol_supported(asked) ? asked : MCP_PROTOCOL_LATEST);
		sb_puts(&r, ",\"capabilities\":{\"tools\":{\"listChanged\":false}},"
			"\"serverInfo\":{\"name\":\"freebee\",\"version\":\"1.0\"},\"instructions\":");
		sb_jstr(&r, text.s);
		sb_putc(&r, '}');
		initialized = true;
		reply(idb.s, r.s);
		sb_free(&r);
		sb_free(&text);
	} else if (strcmp(method, "ping") == 0) {
		reply(idb.s, "{}");
	} else if (strcmp(method, "tools/list") == 0) {
		tools_list(idb.s);
	} else if (strcmp(method, "tools/call") == 0) {
		const char *name = json_get_str(params, "name");
		json *args = json_get(params, "arguments");
		json *tok = json_get(meta, "progressToken");
		sbuf tb;
		sb_init(&tb);
		if (tok)
			json_dump(&tb, tok);
		if (!name)
			reply_error(idb.s, -32602, "tools/call needs a name", NULL);
		else if (strcmp(name, "vm-list") == 0)
			vm_list(idb.s);
		else if (strcmp(name, "vm-wait") == 0)
			vm_wait(idb.s, tok ? tb.s : NULL, args);
		else
			forward(idb.s, line, args);
		sb_free(&tb);
	} else {
		sbuf t;
		sb_init(&t);
		sb_printf(&t, "unknown method '%s'", method);
		reply_error(idb.s, -32601, t.s, NULL);
		sb_free(&t);
	}
out:
	sb_free(&idb);
	json_free(msg);
}

// a line from a VM: its answer (then the call is done) or a progress note
static bool vm_message(pending *p, const char *line, size_t len)
{
	sbuf m;
	sb_init(&m);
	sb_add(&m, line, len);
	json *j = json_parse(line, len, NULL);
	bool answer = j && json_get(j, "id") && !json_get_str(j, "method");
	json_free(j);
	out_line(&m);
	sb_free(&m);
	return answer;
}

int fbmcp_bridge_main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	signal(SIGPIPE, SIG_IGN);
	sbuf in;
	sb_init(&in);
	for (;;) {
		struct pollfd pf[65];
		pending *pp[65];
		int n = 0;
		pf[n].fd = 0;
		pf[n].events = POLLIN;
		pp[n++] = NULL;
		bool waits = false;
		for (pending *p = calls; p && n < 65; p = p->next) {
			if (p->fd < 0) {
				waits = true;
				continue;
			}
			pf[n].fd = p->fd;
			pf[n].events = POLLIN;
			pp[n++] = p;
		}
		int r = poll(pf, n, waits ? 200 : -1);
		if (r < 0 && errno != EINTR)
			break;
		if (r > 0 && (pf[0].revents & (POLLIN | POLLHUP | POLLERR))) {
			char buf[65536];
			ssize_t got = read(0, buf, sizeof(buf));
			if (got == 0 || (got < 0 && errno != EINTR && errno != EAGAIN))
				break;		// the client is gone
			if (got > 0)
				sb_add(&in, buf, got);
			char *nl;
			while (in.len && (nl = memchr(in.s, '\n', in.len)) != NULL) {
				size_t len = nl - in.s;
				*nl = 0;
				if (strspn(in.s, " \t\r") < len)
					client_message(in.s, len);
				memmove(in.s, nl + 1, in.len - len - 1);
				in.len -= len + 1;
			}
		}
		for (int i = 1; r > 0 && i < n; i++) {
			pending *p = pp[i];
			if (!(pf[i].revents & (POLLIN | POLLHUP | POLLERR)))
				continue;
			// the call may have been cancelled meanwhile
			bool still = false;
			for (pending *q = calls; q; q = q->next)
				still |= q == p;
			if (!still)
				continue;
			char buf[65536];
			ssize_t got = read(p->fd, buf, sizeof(buf));
			if (got <= 0) {
				if (got < 0 && errno == EINTR)
					continue;
				reply_text(p->id, "The VM went away during the call: FreeBee quit or crashed.", true);
				drop(p);
				continue;
			}
			sb_add(&p->in, buf, got);
			char *nl;
			bool done = false;
			while (!done && p->in.len && (nl = memchr(p->in.s, '\n', p->in.len)) != NULL) {
				size_t len = nl - p->in.s;
				done = vm_message(p, p->in.s, len);
				memmove(p->in.s, nl + 1, p->in.len - len - 1);
				p->in.len -= len + 1;
			}
			if (done)
				drop(p);
		}
		for (pending *p = calls, *next; p; p = next) {
			next = p->next;
			if (p->fd < 0 && check_wait(p))
				drop(p);
		}
	}
	return 0;
}
