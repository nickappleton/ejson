#include "cop/cop_main.h"
#include "json_simple_load.h"
#include "ejson/json_iface_utils.h"
#include "ejson/ejson.h"
#include <stdarg.h>
#include <stdio.h>

static int unexpected_fail(const char *p_fmt, ...) {
	va_list args;
	fprintf(stderr, "unexpected error: ");
	va_start(args, p_fmt);
	vfprintf(stderr, p_fmt, args);
	va_end(args);
	abort();
	return -1;
}

static void on_parser_error(void *p_context, const struct token_pos_info *p_location, const char *p_format, va_list args) {
	if (p_location != NULL) {
		const char *p_line = p_location->p_line;
		fprintf(p_context, "  on line %d character %d: ", p_location->line_nb, p_location->char_pos);
		vfprintf(p_context, p_format, args);
		printf("    '");
		while (*p_line != '\0' && *p_line != '\n' && *p_line != '\r')
			printf("%c", *p_line++);
		printf("'\n");
		printf("    %*s^\n", p_location->char_pos, "");
	} else {
		fprintf(p_context, "  ");
		vfprintf(p_context, p_format, args);
	}
}

int run_test(const char *p_ejson, const char *p_ref, const char *p_name) {
	struct jnode dut;
	struct evaluation_context ws;
	struct ejson_error_handler err;
	struct cop_salloc_iface alloc;
	struct cop_alloc_grp_temps mem;

	int d;
	
	err.p_context = (p_ref == NULL) ? stdout : stderr;
	err.on_parser_error = on_parser_error;

	cop_alloc_grp_temps_init(&mem, &alloc, 1024, 1024*1024, 16);

	evaluation_context_init(&ws, &alloc);

	if (ejson_load(&dut, &ws, p_ejson, &err)) {
		if (p_ref != NULL) {
			fprintf(stderr, "FAILED: test '%s' failed due to above messages.\n", p_name);
			return 1;
		} else {
			printf("PASSED: xtest '%s'\n", p_name);
		}
	} else if (p_ref == NULL) {
		printf("FAILED: xtest '%s' generated a node.\n", p_name);
		return 1;
	}

	if (p_ref != NULL) {
		struct jnode ref;
		struct cop_salloc_iface a1;
		struct cop_alloc_grp_temps m1;
		struct cop_salloc_iface a2;
		struct cop_alloc_grp_temps m2;
		struct cop_salloc_iface a3;
		struct cop_alloc_grp_temps m3;

		cop_alloc_grp_temps_init(&m1, &a1, 1024, 1024*1024, 16);
		cop_alloc_grp_temps_init(&m2, &a2, 1024, 1024*1024, 16);
		cop_alloc_grp_temps_init(&m3, &a3, 1024, 1024*1024, 16);

		if (parse_json(&ref, &a1, &a2, p_ref))
			return unexpected_fail("could not parse reference JSON:\n  %s\n", p_ref);

		if ((d = are_different(&ref, &dut, &a3)) < 0) {
			return unexpected_fail("are_different failed to execute\n");
		}
		
		if (d) {
			fprintf(stderr, "FAILED: test '%s':\n", p_name);
			fprintf(stderr, "  Reference:\n    ");
			jnode_print(&ref, &a2, 4);
			fprintf(stderr, "  DUT:\n    ");
			jnode_print(&dut, &a2, 4);
			return 1;
		}

		printf("PASSED: test '%s'.\n", p_name);
	}

	return 0;
}

static int test_main(int argc, char *argv[]) {
	int errors = 0;
	int tests = 0;

	/* Simple JSON types */
	tests++; errors += run_test
		("\"hello world\""
		,"\"hello world\""
		,"string objects"
		);
	tests++; errors += run_test
		("null"
		,"null"
		,"positive null object"
		);
	tests++; errors += run_test
		("true"
		,"true"
		,"positive true boolean object"
		);
	tests++; errors += run_test
		("false"
		,"false"
		,"positive false boolean object"
		);
	tests++; errors += run_test
		("5"
		,"5"
		,"positive int objects"
		);
	tests++; errors += run_test
		("5.0"
		,"5.0"
		,"positive real objects"
		);
	tests++; errors += run_test
		("-5"
		,"-5"
		,"negative int objects"
		);
	tests++; errors += run_test
		("-5.0"
		,"-5.0"
		,"negative real objects"
		);
	tests++; errors += run_test
		("[]"
		,"[]"
		,"empty list"
		);
	tests++; errors += run_test
		("{}"
		,"{}"
		,"empty dictionary"
		);
	tests++; errors += run_test
		("{\"hello1\": null}"
		,"{\"hello1\": null}"
		,"dictionary with a single null key"
		);
	tests++; errors += run_test
		("{\"hello1\": {\"uhh\": null, \"thing\": 100}}"
		,"{\"hello1\": {\"uhh\": null, \"thing\": 100}}"
		,"dictionary nesting"
		);
	tests++; errors += run_test
		("[1,-2,3.4,-4.5,5.6e2,-7.8e-2]"
		,"[1,-2,3.4,-4.5,5.6e2,-7.8e-2]"
		,"numeric objects in a list"
		);

	/* Hexadecimal numeric extensions */
	tests++; errors += run_test
		("0x01"
		,"1"
		,"hex int objects"
		);
	tests++; errors += run_test
		("0x20"
		,"32"
		,"hex int objects"
		);
	tests++; errors += run_test
		("0x0a"
		,"10"
		,"hex int objects"
		);
	tests++; errors += run_test
		("0x4F"
		,"79"
		,"hex int objects"
		);

	/* Binary and unary expression tests */
	tests++; errors += run_test
		("5+5+5"
		,"15"
		,"int additive expression 1"
		);
	tests++; errors += run_test
		("5+5-5"
		,"5"
		,"int additive expression 2"
		);
	tests++; errors += run_test
		("5-5-5"
		,"-5"
		,"int additive expression 3"
		);
	tests++; errors += run_test
		("5+5.0-5"
		,"5.0"
		,"promotion additive expression 1"
		);
	tests++; errors += run_test
		("5-5.0-5"
		,"-5.0"
		,"promotion additive expression 2"
		);
	tests++; errors += run_test
		("1.0+5.0"
		,"6.0"
		,"float additive expression 1"
		);
	tests++; errors += run_test
		("1+2*3+4"
		,"11"
		,"test precedence of addition is lower than multiplication"
		);
	tests++; errors += run_test
		("3*2^3"
		,"24.0"
		,"test precedence of multiplication is lower than exponentiation"
		);
	tests++; errors += run_test
		("0+-2^3"
		,"-8.0"
		,"test precedence of unary negation is lower than exponentiation"
		);
	tests++; errors += run_test
		("not true"
		,"false"
		,"test negation of true"
		);
	tests++; errors += run_test
		("not false"
		,"true"
		,"test negation of false"
		);
	tests++; errors += run_test
		("not true or not false"
		,"true"
		,"test precedence of logical not is higher than logical or"
		);
	tests++; errors += run_test
		("not false and true"
		,"true"
		,"test precedence of logical not is higher than logical and"
		);
	tests++; errors += run_test
		("3+1>=1+4"
		,"false"
		,"comparison expression"
		);
	tests++; errors += run_test
		("3+2<=1+4"
		,"true"
		,"comparison expression"
		);
	tests++; errors += run_test
		("true==true"
		,"true"
		,"comparison expression"
		);
	tests++; errors += run_test
		("true==false"
		,"false"
		,"comparison expression"
		);

	/* List concatenations */
	tests++; errors += run_test
		("[1,2,3,4]+[5,6,7]+[8,9,10]"
		,"[1,2,3,4,5,6,7,8,9,10]"
		,"list concatenation"
		);
	tests++; errors += run_test
		("[1,2,3,4]+(range [5,8])+[9,10]"
		,"[1,2,3,4,5,6,7,8,9,10]"
		,"list concatenation incl. a range"
		);
	tests++; errors += run_test
		("(call func[] [1,2,3,4] [])+(range [5,8])+[9,10]"
		,"[1,2,3,4,5,6,7,8,9,10]"
		,"list concatenation incl. a range and function"
		);

	/* range tests */
	tests++; errors += run_test
		("range[5]"
		,"[0, 1, 2, 3, 4]"
		,"range generator simple"
		);
	tests++; errors += run_test
		("range[6,11]"
		,"[6, 7, 8, 9, 10, 11]"
		,"range generator from-to"
		);
	tests++; errors += run_test
		("range[6,-11]"
		,"[6,5,4,3,2,1,0,-1,-2,-3,-4,-5,-6,-7,-8,-9,-10,-11]"
		,"range generator from-to reverse"
		);
	tests++; errors += run_test
		("range[6,2,10]"
		,"[6,8,10]"
		,"range generator from-step-to"
		);
	tests++; errors += run_test
		("range[6,-3,-9]"
		,"[6,3,0,-3,-6,-9]"
		,"range generator from-step-to 2"
		);

	/* function tests */
	tests++; errors += run_test
		("call func[] 1 []"
		,"1"
		,"calling a function that takes no arguments and returns 1"
		);
	tests++; errors += run_test
		("call func[x] x [55]"
		,"55"
		,"calling a function that takes one argument and returns its value"
		);
	tests++; errors += run_test
		("call func[x] [x] [55]"
		,"[55]"
		,"calling a function that takes one argument and returns its value "
		 "in a list"
		);
	tests++; errors += run_test
		("call func[x, y, z] x * y + z [3, 5, 7]"
		,"22"
		,"calling a function that multiplies the first two arguments and "
		 "adds the third (test order of arguments on stack)"
		);
	tests++; errors += run_test
		("call func[x, y] call func[z] x - y * z [3] [5, 7]"
		,"-16"
		,"calling a function that contains another function (nested stack "
		 "access test)"
		);
	tests++; errors += run_test
		("define fz = func[x, y, z] x - y * z; call func[x] call func[y] call fz [x, y, 3] [5] [7]"
		,"-8"
		,"triple nested function call calling a workspace defined function (test stack behavior when calling defined function)"
		);
	tests++; errors += run_test
		("call func[x] call func[y] call func[z] x - y * z [3] [5] [7]"
		,"-8"
		,"triple nested function call"
		);
	tests++; errors += run_test
		("call func[y] call y [] [func[] 111]"
		,"111"
		,"calling a function that calls the given function passed as an "
		 "argument"
		);
	tests++; errors += run_test
		("call func[x] [1, x, 2, 3] [50]"
		,"[1, 50, 2, 3]"
		,"a function that returns a 4 element list with the second element "
		 "equal to the argument"
		);
	tests++; errors += run_test
		("call func[x, y, z] x * y + z call func[x] [3, 5, x] [7]"
		,"22"
		,"calling a function where the arguments are the list produced by "
		 "calling another function"
		);
	tests++; errors += run_test
		("call func[x, y, z] x * y + z range[4, 6]"
		,"26"
		,"calling a function where the arguments are the list produced by "
		 "calling range"
		);
	tests++; errors += run_test
		("call call call func [x] func [y] func[z] x-y-z [13] [7] [5]"
		,"1"
		,"CCCFFF defined function closure test"
		);
	tests++; errors += run_test
		("call call func [x] call func [y] func[z] x-y-z [13] [7] [5]"
		,"-11"
		,"CCFCFF defined function closure test"
		);
	tests++; errors += run_test
		("call call func [x] func [y] call func[z] x-y-z [13] [7] [5]"
		,"-11"
		,"CCFFCF defined function closure test"
		);
	tests++; errors += run_test
		("call func [x] call call func [y] func[z] x-y-z [13] [7] [5]"
		,"-15"
		,"CFCCFF defined function closure test"
		);
	tests++; errors += run_test
		("call func [x] call func [y] call func[z] x-y-z [13] [7] [5]"
		,"-15"
		,"CFCFCF defined function closure test"
		);
	tests++; errors += run_test
		("call call func [x] func [y] y+x [10] [1]"
		,"11"
		,"CCFF defined function closure test"
		);
	tests++; errors += run_test
		("define f = func [a] (func [b] b-a);\n"
		 "define sub_10 = call f [10];\n"
		 "call sub_10 [1]"
		,"-9"
		,"defined function closure test"
		);
	tests++; errors += run_test
		("define f = func [a] (func [b] b-a);\n"
		 "define sub_1 = call f [1];\n"
		 "define sub_10 = call f [10];\n"
		 "[call sub_10 [7], call sub_1 [2]]"
		,"[-3, 1]"
		,"defined function closure test"
		);

	tests++; errors += run_test
		("call access [func [a,b] a*b, func [a,b] a-b] 1 [1,2]"
		,"-1"
		,"list of functions"
		);
	tests++; errors += run_test
		("map func [x] call x [3,5] [func [a,b] a*b, func [a,b] a-b, func [a,b] a%b]"
		,"[15,-2,3]"
		,"map of functions"
		);
	tests++; errors += run_test
		("call\n"
		 "  func [c]\n"
		 "    map\n"
		 "      func [x]\n"
		 "        call x [3, 5]\n"
		 "      [func [a, b] c+a*b\n"
		 "      ,func [a, b] c+a-b\n"
		 "      ,func [a, b] c+a%b\n"
		 "      ]\n"
		 "  [10]"
		,"[25,8,13]"
		,"map of functions in a call"
		);

	/* define tests */
	tests++; errors += run_test
		("define x = 11; define y = 7; x * y"
		,"77"
		,"use a workspace variable"
		);
	tests++; errors += run_test
		("define x = func[z] z*z; define y = 7; call x [y]"
		,"49"
		,"use a workspace variable as a function"
		);

	/* access tests */
	tests++; errors += run_test
		("access [1,2,3] 1"
		,"2"
		,"access of a value from a literal list"
		);
	tests++; errors += run_test
		("access range[10] 4"
		,"4"
		,"access an element of a generated list"
		);
	tests++; errors += run_test
		("access call func[x] [1, x, 2, 3] [50] 1"
		,"50"
		,"access an element of the list returned by a function"
		);
	tests++; errors += run_test
		("access {\"value1\": true, \"value2\": 399, \"value3\": false} \"value2\""
		,"399"
		,"access of dictionary item"
		);
	tests++; errors += run_test
		("access {\"value1\": true, \"value2\": 399, \"value3\": false} call func [x] format [\"value%d\", x] [3]"
		,"false"
		,"access of dictionary item where the key is generated using format"
		);

	/* map tests */
	tests++; errors += run_test
		("call func[y] map func[x] [1, x, x*x] [y+1] [3]"
		,"[[1,4,16]]"
		,"advanced map/function test 1"
		);
	tests++; errors += run_test
		("call func[y] map func[x] [1, x, x*x] range[1,y] [3]"
		,"[[1,1,1],[1,2,4],[1,3,9]]"
		,"advanced map/function test 2"
		);
	tests++; errors += run_test
		("call func[x] call func[y] map func[z] [1, z, z*z] [y-1] [x-2] [4]"
		,"[[1,1,1]]"
		,"advanced map/function test 3"
		);
	tests++; errors += run_test
		("define far_call = func[z] [1, z, z*z]; call func[x] call func[y] map far_call [y-1] [x-2] [4]"
		,"[[1,1,1]]"
		,"advanced map/function test 3 (inner far call)"
		);
	tests++; errors += run_test
		("map func[x] [1, x, x*x] [1,2,3]"
		,"[[1,1,1],[1,2,4],[1,3,9]]"
		,"map operation basics"
		);
	tests++; errors += run_test
		("map func[x] access [\"a\",\"b\",\"c\",\"d\",\"e\"] x%5 range[-2,1,8]"
		,"[\"d\",\"e\",\"a\",\"b\",\"c\",\"d\",\"e\",\"a\",\"b\",\"c\",\"d\"]"
		,"map over a range basics"
		);
	tests++; errors += run_test
		("map func[x] range[x] range[0,5]"
		,"[[],[0],[0,1],[0,1,2],[0,1,2,3],[0,1,2,3,4]]"
		,"use map to generate a list of incrementing ranges over a range"
		);
	tests++; errors += run_test
		("range call func[] [1,2,9] []"
		,"[1,3,5,7,9]"
		,"call range with arguments given by the result of a function call"
		);
	tests++; errors += run_test
		("map func[x] x <= 2 range [5]"
		,"[true, true, true, false, false]"
		,"map of comparison result"
		);
	tests++; errors += run_test
		("call call call func [a, b] func [c] func [d, e, f] [a, b, c, d, e, f] [1, 2] [3] [4, 5, 6]"
		,"[1,2,3,4,5,6]"
		,"order of nested function arguments pushed onto the stack"
		);
	tests++; errors += run_test
		("call func [a] (map func [c] c * a [3, 5]) [1]"
		,"[3, 5]"
		,"mixing of call and map"
		);
	tests++; errors += run_test
		("call func [a, b] (map func [c] c * a + b [3, 5]) [1, 2]"
		,"[5, 7]"
		,"mixing of call and map"
		);

	/* format tests */
	tests++; errors += run_test
		("format[\"hello\"]"
		,"\"hello\""
		,"test format with no arguments"
		);
	tests++; errors += run_test
		("format[\"hello %%\"]"
		,"\"hello %\""
		,"format escapeing %% properly"
		);
	tests++; errors += run_test
		("format[\"hello %d %d\", 1, 2000]"
		,"\"hello 1 2000\""
		,"test format with two integer arguments"
		);
	tests++; errors += run_test
		("format[\"%d-%s.wav\", 36, \"c\"]"
		,"\"36-c.wav\""
		,"test format with an integer and string argument"
		);
	tests++; errors += run_test
		("map func[x] format[\"%03d-%s.wav\", x, access [\"c\", \"d\", \"e\"] x%3] range[36,40]"
		,"[\"036-c.wav\", \"037-d.wav\", \"038-e.wav\", \"039-c.wav\", \"040-d.wav\"]"
		,"test using format to generate mapped strings"
		);
	tests++; errors += run_test
		("call access [func[x] x+1, func[x] x+2, func[x] x+3] 1 [10]"
		,"12"
		,"test calling a function that is in a list of functions"
		);

	/* FIXME: something is broken i think with the test comparison function. */
	tests++; errors += run_test
		("define notes=[\"a\",\"b\",\"c\"];\n"
		 "map func[x]\n"
		 "  {\"name\": access notes x % 3, \"id\": x} range[0,5]\n"
		,"[{\"id\":0,\"name\":\"a\"}"
		 ",{\"id\":1,\"name\":\"b\"}"
		 ",{\"id\":2,\"name\":\"c\"}"
		 ",{\"id\":3,\"name\":\"a\"}"
		 ",{\"id\":4,\"name\":\"b\"}"
		 ",{\"id\":2,\"name\":\"c\"}"
		 "]"
		,"use map to generate a list of dicts"
		);

	/* if tests */
	tests++; errors += run_test
		("if 1>2 \"yes\" \"no\""
		,"\"no\""
		,"test if with a greater than condition that is false"
		);
	tests++; errors += run_test
		("if 6*3 == 18 \"yes\" \"no\""
		,"\"yes\""
		,"test if with a greater than condition that is false"
		);
	tests++; errors += run_test
		("map func [x] if x>0 x 1 [0, 1, 2, 3]"
		,"[1, 1, 2, 3]"
		,"test if with a greater than condition that is false"
		);




	/* expected fail tests due to bad parsing syntax */

	/* func error tests */
	tests++; errors += run_test
		("func"
		,NULL
		,"failure because need more tokens"
		);
	tests++; errors += run_test
		("func sadsa 1"
		,NULL
		,"func expects open parenthesis"
		);
	tests++; errors += run_test
		("func [sadsa 1"
		,NULL
		,"func expects comma or close parenthesis"
		);
	tests++; errors += run_test
		("func [sadsa, 1"
		,NULL
		,"func arguments must be literals"
		);
	tests++; errors += run_test
		("func [sadsa 1]"
		,NULL
		,"func expects a function body"
		);
	tests++; errors += run_test
		("func [sadsa 1] ["
		,NULL
		,"func cannot parse function body"
		);
	tests++; errors += run_test
		("func [sadsa, sadsa] 1"
		,NULL
		,"func arguments must not alias each other"
		);
	tests++; errors += run_test
		("define sadsa = 1; func [sadsa] 1"
		,NULL
		,"func arguments must not alias workspace variables"
		);
	
	/* access error tests */
	tests++; errors += run_test
		("access"
		,NULL
		,"access no tokens for first expression"
		);
	tests++; errors += run_test
		("access [1"
		,NULL
		,"access could not parse first expression"
		);
	tests++; errors += run_test
		("access 1"
		,NULL
		,"access no tokens for second expression"
		);
	tests++; errors += run_test
		("access 1 [1"
		,NULL
		,"access could not parse second expression"
		);

	/* map tests */
	tests++; errors += run_test
		("map"
		,NULL
		,"map no tokens for first expression"
		);
	tests++; errors += run_test
		("map [1"
		,NULL
		,"map could not parse first expression"
		);
	tests++; errors += run_test
		("map 1"
		,NULL
		,"map no tokens for second expression"
		);
	tests++; errors += run_test
		("map 1 [1"
		,NULL
		,"map could not parse second expression"
		);

	/* access error tests */
	tests++; errors += run_test
		("access 1 \"hehre\""
		,NULL
		,"the list expression for access did not evaluate to a list or a dictionary"
		);
	tests++; errors += run_test
		("access [1, 2, 3, 4, 5] \"hehre\""
		,NULL
		,"access of list item using a non-integer key"
		);
	tests++; errors += run_test
		("access {\"value1\": true, \"value2\": 399, \"value3\": false} 100"
		,NULL
		,"access of dictionary item using a non-string key"
		);
	tests++; errors += run_test
		("access {\"value1\": true, \"value2\": 399, \"value3\": false} \"hehre\""
		,NULL
		,"access of a missing dictionary item"
		);

	/* end of document test */
	tests++; errors += run_test
		("1 1"
		,NULL
		,"expected no more tokens"
		);

	/* define error tests */
	tests++; errors += run_test
		("define"
		,NULL
		,"out of tokens"
		);
	tests++; errors += run_test
		("define 1"
		,NULL
		,"define expects a literal argument"
		);
	tests++; errors += run_test
		("define hello FAIL"
		,NULL
		,"define expects an equals"
		);
	tests++; errors += run_test
		("define hello = 1 FAIL"
		,NULL
		,"define expects a semicolon"
		);

	/* lparen error tests */
	tests++; errors += run_test
		("("
		,NULL
		,"expect expression after ("
		);
	tests++; errors += run_test
		("9 + 8 * k"
		,NULL
		,"failure to parse rhs due to identifier not existing"
		);
	tests++; errors += run_test
		("(1,"
		,NULL
		,"failure because expect )"
		);
	tests++; errors += run_test
		("["
		,NULL
		,"failure because need more tokens"
		);
	tests++; errors += run_test
		("[1,ggg"
		,NULL
		,"failure because need more tokens"
		);


	/* expected evaluation time failure tests due to bad algorithm */

	/* map failures */

	tests++; errors += run_test
		("map 1 [1,2,3]"
		,NULL
		,"map expects a function argument that takes one argument"
		);
	tests++; errors += run_test
		("map func[] 1 [1,2,3]"
		,NULL
		,"map expects a function argument that takes one argument"
		);
	tests++; errors += run_test
		("map func[x, y] 1 [1,2,3]"
		,NULL
		,"map expects a function argument that takes one argument"
		);
	tests++; errors += run_test
		("map func[x] x {}"
		,NULL
		,"map expected a list argument following the function"
		);

	/* range error tests */

	tests++; errors += run_test
		("range 1"
		,NULL
		,"range expects a list argument"
		);
	tests++; errors += run_test
		("range []"
		,NULL
		,"range expects between 1 and 3 arguments"
		);
	tests++; errors += run_test
		("range [1,2,3,4]"
		,NULL
		,"range expects between 1 and 3 arguments"
		);

	tests++; errors += run_test
		("1+\"a\""
		,NULL
		,"arguments to operators must be integers of floats"
		);
	tests++; errors += run_test
		("-\"a\""
		,NULL
		,"arguments to unary negate must be an integer or a float"
		);
	tests++; errors += run_test
		("{1: null}"
		,NULL
		,"dictionary keys must evaluate to strings"
		);
	tests++; errors += run_test
		("define x = call func[] 1 [];\n {x: null}"
		,NULL
		,"dictionary keys must evaluate to strings"
		);
	tests++; errors += run_test
		("func[x] x"
		,NULL
		,"the evaluation of the root node cannot be a function"
		);
	tests++; errors += run_test
		("call func[] 1 [1, 2]"
		,NULL
		,"call a function with incorrect number of arguments (0)"
		);
	tests++; errors += run_test
		("call func[x] x [1, 2]"
		,NULL
		,"call a function with incorrect number of arguments (1)"
		);
	tests++; errors += run_test
		("{\"a\": 1, \"a\": 2}"
		,NULL
		,"attempted to add a key to a dictionary that already existed"
		);

	tests++; errors += run_test
		(""
		,NULL
		,"empty document should result in a parse error"
		);

	/* test if with non-bool argument */
	tests++; errors += run_test
		("if 1 42 43"
		,NULL
		,"first argument to if must be a boolean"
		);

	tests++; errors += run_test
		("call func [x] x [x]"
		,NULL
		,"x should not be in scope here"
		);

	fprintf((errors) ? stderr : stdout, "\n%d of %d tests passed\n", tests - errors, tests);

	return (errors) ? EXIT_FAILURE : EXIT_SUCCESS;
}

COP_MAIN(test_main)

