#include "ejson/ejson.h"
#include "ejson/json_iface_utils.h"

static void on_parser_error(void *p_context, const struct token_pos_info *p_location, const char *p_format, va_list args) {
	if (p_location != NULL) {
		const char *p_line = p_location->p_line;
		fprintf(stderr, "  on line %d character %d: ", p_location->line_nb, p_location->char_pos);
		vfprintf(stderr, p_format, args);
		fprintf(stderr, "    '");
		while (*p_line != '\0' && *p_line != '\n' && *p_line != '\r')
			fprintf(stderr, "%c", *p_line++);
		fprintf(stderr, "'\n");
		fprintf(stderr, "    %*s^\n", p_location->char_pos, "");
	} else {
		fprintf(stderr, "  ");
		vfprintf(stderr, p_format, args);
	}
}


static void *load_text_to_memory(const char *fname) {
	FILE *f = fopen(fname, "rb");
	if (f != NULL) {
		if (fseek(f, 0, SEEK_END) == 0) {
			long fsz = ftell(f);
			if (fsz >= 0) {
				if (fseek(f, 0, SEEK_SET) == 0) {
					unsigned char *fbuf = malloc(fsz + 1);
					if (fbuf != NULL) {
						if (fread(fbuf, 1, fsz, f) == fsz) {
							fclose(f);
							fbuf[fsz] = '\0';
							return fbuf;
						}
						free(fbuf);
					}
				}
			}
		}
		fclose(f);
	}
	return NULL;
}


int main(int argc, char *argv[]) {

	if (argc > 1) {
		char *data;
		struct jnode dut;
		struct evaluation_context ws;
		struct ejson_error_handler err;
		struct linear_allocator alloc;

		err.on_parser_error = on_parser_error;
		err.p_context = NULL;

		evaluation_context_init(&ws);

		if ((data = load_text_to_memory(argv[1])) == NULL) {
			fprintf(stderr, "failed to load file\n");
			return EXIT_FAILURE;
		}

		if (ejson_load(&dut, &ws, data, &err)) {
			fprintf(stderr, "failed to parse document\n");
			return EXIT_FAILURE;
		}

		if (jnode_print(&dut, &alloc, 0)) {
			fprintf(stderr, "failed to print root node\n");
			return EXIT_FAILURE;
		}



		free(data);
	}
	


	return 0;
}
