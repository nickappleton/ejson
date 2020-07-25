#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ejson/ejson.h"
#include "ejson/json_iface_utils.h"

static void on_parser_error(void *p_context, const struct token_pos_info *p_location, const char *p_format, va_list args) {
	if (p_location != NULL) {
		const char *p_line = p_location->p_line;
		fprintf(stdout, "  on line %d character %d: ", p_location->line_nb, p_location->char_pos);
		vfprintf(stdout, p_format, args);
		fprintf(stdout, "    '");
		while (*p_line != '\0' && *p_line != '\n' && *p_line != '\r')
			fprintf(stdout, "%c", *p_line++);
		fprintf(stdout, "'\n");
		fprintf(stdout, "    %*s^\n", p_location->char_pos, "");
	} else {
		fprintf(stdout, "  ");
		vfprintf(stdout, p_format, args);
	}
}

int main(int argc, char **argv) {
	setlinebuf(stdin);
	setlinebuf(stdout);

	char   buf[65536];
	size_t buf_offset = 0;

	fprintf(stdout, "> ");
	fflush(stdout);

	while (fgets(buf + buf_offset, sizeof(buf) - buf_offset, stdin)) {
		struct ejson_error_handler err;
		struct cop_salloc_iface alloc;
		struct cop_alloc_grp_temps mem;
		struct evaluation_context ws;
		struct jnode node;

		size_t sl = strlen(buf + buf_offset);
		if (!sl)
			continue;
		buf_offset += sl;
		if (buf_offset>1 && buf[buf_offset-2] == '\\') {
			buf[buf_offset-2] = '\n';
			buf[buf_offset-1] = '\0';
			buf_offset--;
			fprintf(stdout, "> ");
			fflush(stdout);
			continue;
		}
		if (buf_offset>1) {
			buf[buf_offset-1] = '\0';
		}
		buf_offset = 0;

		err.on_parser_error = on_parser_error;
		err.p_context       = NULL;

		if (cop_alloc_grp_temps_init(&mem, &alloc, 1024, 1024*1024, 16)) {
			abort();
		}

		evaluation_context_init(&ws, &alloc);

		if (ejson_load(&node, &ws, buf, &err)) {
			fprintf(stdout, "failed to parse document\n> ");
			continue;
		}

		if (jnode_print(&node, &alloc, 0)) {
			fprintf(stdout, "failed to print root node\n> ");
			continue;
		}

		fprintf(stdout, "\n> ");
		fflush(stdout);
	}

	return 0;
};