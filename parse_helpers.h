#ifndef PARSE_HELPERS_H
#define PARSE_HELPERS_H

static int expect_decimal_digit(const char **pp_buf, unsigned *digit) {
	const char *pb = *pp_buf;
	char b = *pb++;
	if (b >= '0' && b <= '9') {
		*digit  = b - '0';
	} else {
		return 1;
	}
	*pp_buf = pb;
	return 0;
}

static int expect_hex_digit(const char **pp_buf, unsigned *h) {
	const char *pb = *pp_buf;
	char b = *pb++;
	if (b >= '0' && b <= '9') {
		*h = b - '0';
	} else if (b >= 'a' && b <= 'f') {
		*h = 10 + (b - 'a');
	} else if (b >= 'A' && b <= 'F') {
		*h = 10 + (b - 'A');
	} else {
		return 1;
	}
	*pp_buf = pb;
	return 0;
}

static int expect_hex_digit_accumulate(const char **buf, unsigned *h) {
	unsigned v;
	if (expect_hex_digit(buf, &v))
		return -1;
	*h   = *h * 16 + v;
	return 0;
}

static int expect_char(const char **pp_buf, char c) {
	const char *p_buf = *pp_buf;
	if (*p_buf != c)
		return 1;
	*pp_buf = p_buf + 1;
	return 0;
}

static int expect_whitespace(const char **pp_buf) {
	const char *p_buf = *pp_buf;
	if (*p_buf == '\t' || *p_buf == '\r' || *p_buf == '\n' || *p_buf == ' ') {
		*pp_buf = p_buf + 1;
		return 0;
	}
	return -1;
}

static void eat_whitespace(const char **pp_buf) {
	while (!expect_whitespace(pp_buf));
}

static int expect_num(const char **pp_buf, unsigned long long *p_num) {
	unsigned long long num;
	unsigned d;
	if (expect_decimal_digit(pp_buf, &d))
		return -1;
	num = d;
	while (!expect_decimal_digit(pp_buf, &d))
		num = num * 10 + d;
	*p_num = num;
	return 0;
}

static int expect_consecutive(const char **pp_buf, const char *p_expect) {
	const char *p_buf = *pp_buf;
	while (*p_expect != '\0' && *p_buf == *p_expect) {
		p_buf++;
		p_expect++;
	}
	if (*p_expect != '\0')
		return -1;
	*pp_buf = p_buf;
	return 0;
}

static int is_eof(const char **pp_buf) {
	const char *p_buf = *pp_buf;
	return *p_buf == '\0';
}


#endif /* PARSE_HELPERS_H */
