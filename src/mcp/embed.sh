#!/bin/sh
# Turn the MCP tool list and instructions into C strings (schema_data.h).
cd "$(dirname "$0")" || exit 1
emit() {
	echo "static const char $1[] ="
	sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$/\\n"/' "$2"
	echo ";"
}
echo "/* made by embed.sh from tools.json and instructions.txt: do not edit */"
emit tools_src tools.json
emit instructions_src instructions.txt
