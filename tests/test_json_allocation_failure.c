#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "arraylist.h"
#include "json_object_private.h"

#if !defined(JSON_ALLOCATION_SAFETY) || JSON_ALLOCATION_SAFETY != 1
#error "JSON allocation safety API marker missing"
#endif

static size_t allocation_calls;
static size_t fail_on_call;
static enum json_tokener_error parse_error;

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *ptr, size_t size);
char *__real_strdup(const char *text);

static int should_fail(const char *kind)
{
	allocation_calls++;
	if (fail_on_call != 0 && allocation_calls == fail_on_call) return 1;
	(void)kind;
	return 0;
}

void *__wrap_malloc(size_t size)
{
	if (should_fail("malloc")) return NULL;
	return __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size)
{
	if (should_fail("calloc")) return NULL;
	return __real_calloc(count, size);
}

void *__wrap_realloc(void *ptr, size_t size)
{
	if (should_fail("realloc")) return NULL;
	return __real_realloc(ptr, size);
}

char *__wrap_strdup(const char *text)
{
	if (should_fail("strdup")) return NULL;
	return __real_strdup(text);
}

static void require(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "json allocation regression: %s\n", message);
		exit(1);
	}
}

static void inject_at(size_t nth)
{
	allocation_calls = 0;
	fail_on_call = nth;
}

static void stop_injection(void)
{
	fail_on_call = 0;
	allocation_calls = 0;
}

typedef struct json_object *(*constructor_fn)(void);

static void test_constructor_failures(constructor_fn make, size_t allocation_count,
				     const char *label)
{
	for (size_t nth = 1; nth <= allocation_count; nth++) {
		struct json_object *obj;
		inject_at(nth);
		obj = make();
		require(obj == NULL, label);
		require(allocation_calls == nth, "constructor stops at failed allocation");
		stop_injection();
		obj = make();
		require(obj != NULL, "constructor succeeds on retry");
		json_object_put(obj);
	}
}

static struct json_object *new_object(void)
{
	return json_object_new_object();
}

static struct json_object *new_array(void)
{
	return json_object_new_array();
}

static struct json_object *new_string(void)
{
	return json_object_new_string("allocation");
}

static struct json_object *new_string_len(void)
{
	return json_object_new_string_len("bounded", 7);
}

static void require_parseable(const char *text)
{
	enum json_tokener_error error = json_tokener_success;
	struct json_object *parsed = json_tokener_parse_verbose(text, &error);
	require(parsed != NULL && error == json_tokener_success,
		"normal serialization remains valid JSON");
	json_object_put(parsed);
}

static void test_string_allocations(void)
{
	struct json_object *value;
	const char *text;

	test_constructor_failures(new_string, 2, "new_string handles allocation failure");
	test_constructor_failures(new_string_len, 2,
				  "new_string_len handles allocation failure");
	require(json_object_new_string_len("x", -1) == NULL,
		"negative string length is rejected");
	value = json_object_new_string_len("abc", 2);
	require(value != NULL, "bounded string constructor");
	text = json_object_get_string(value);
	require(text && memcmp(text, "ab\0", 3) == 0,
		"length-bounded strings have a terminator");
	json_object_put(value);
}

static void test_object_insert_failures(void)
{
	for (size_t nth = 1; nth <= 4; nth++) {
		struct json_object *obj = json_object_new_object();
		struct json_object *value = json_object_new_int(22);
		const char *serialized;
		require(obj != NULL && value != NULL, "prepare object insertion fixture");
		json_object_object_add(obj, "first", json_object_new_int(11));
		require(json_object_object_get(obj, "first") != NULL,
			"first object member inserted");

		inject_at(nth);
		json_object_object_add(obj, "second", value);
		stop_injection();
		if (nth <= 3) {
			require(json_object_to_json_string(obj) == NULL,
				"failed key/table insertion marks object unserializable");
			require(json_object_get_int(json_object_object_get(obj, "first")) == 11,
				"failed resize preserves old table contents");
		} else {
			serialized = json_object_to_json_string(obj);
			require(serialized != NULL, "object insertion succeeds after failure positions");
			require_parseable(serialized);
			require(json_object_get_int(json_object_object_get(obj, "second")) == 22,
				"successful object insertion retains the value");
		}
		json_object_put(obj);
	}

	{
		struct json_object *obj = json_object_new_object();
		json_object_object_add(NULL, "ignored", json_object_new_string("consumed"));
		require(obj != NULL, "object null-parent ownership fixture");
		json_object_object_add(obj, NULL, json_object_new_int(1));
		require(json_object_to_json_string(obj) == NULL,
			"invalid object member marks its parent failed");
		json_object_put(obj);
	}

	{
		struct json_object *obj = json_object_new_object();
		struct json_object *value = json_object_new_int(7);
		require(obj != NULL && value != NULL, "same-value replacement fixture");
		json_object_object_add(obj, "value", value);
		value = json_object_get(json_object_object_get(obj, "value"));
		require(value && value->_ref_count == 2, "get adds caller ownership reference");
		json_object_object_add(obj, "value", value);
		require(value->_ref_count == 1,
			"same-value replacement releases old container reference");
		json_object_put(obj);
	}
}

static void test_array_insert_failures(void)
{
	struct json_object *array = json_object_new_array();
	struct json_object *value;
	require(array != NULL, "array allocation");
	for (int i = 0; i < json_object_get_array(array)->size; i++)
		require(json_object_array_add(array, json_object_new_int(i)) == 0,
			"fill initial array storage");
	value = json_object_new_int(9);
	require(value != NULL && json_object_array_add(array, value) == 0,
		"array same-value replacement fixture");
	value = json_object_get(json_object_array_get_idx(array, json_object_array_length(array) - 1));
	require(value && value->_ref_count == 2, "array get adds caller ownership reference");
	require(json_object_array_put_idx(array, json_object_array_length(array) - 1, value) == 0,
		"array replacement accepts the referenced same value");
	require(value->_ref_count == 1,
		"same-value array replacement releases old container reference");
	while (json_object_array_length(array) < json_object_get_array(array)->size)
		require(json_object_array_add(array, json_object_new_int(0)) == 0,
			"refill array storage after same-value replacement");
	value = json_object_new_string("failed expansion value");
	require(value != NULL, "array value before injected failure");
	inject_at(1);
	require(json_object_array_add(array, value) == -1,
		"array append reports expansion failure");
	stop_injection();
	require(json_object_to_json_string(array) == NULL,
		"failed array append marks array unserializable");
	json_object_put(array);

	array = json_object_new_array();
	value = json_object_new_int(1);
	require(array != NULL && value != NULL, "array put failure fixture");
	require(json_object_array_put_idx(array, INT_MAX, value) == -1,
		"unrepresentable array index is rejected");
	require(json_object_to_json_string(array) == NULL,
		"failed array put marks array unserializable");
	json_object_put(array);
}

static void test_nested_error_and_growth(void)
{
	struct json_object *parent = json_object_new_object();
	struct json_object *child = json_object_new_array();
	struct json_object *value;
	const char *serialized;
	require(parent != NULL && child != NULL, "nested serialization fixture");
	for (int i = 0; i < json_object_get_array(child)->size; i++)
		require(json_object_array_add(child, json_object_new_int(i)) == 0,
			"fill nested array");
	value = json_object_new_int(json_object_get_array(child)->size);
	require(value != NULL, "prepare nested allocation failure value");
	inject_at(1);
	require(json_object_array_add(child, value) == -1,
		"injected nested array allocation failure");
	stop_injection();
	json_object_object_add(parent, "child", child);
	require(json_object_to_json_string(parent) == NULL,
		"nested serializer propagates the child error");
	json_object_put(parent);

	parent = json_object_new_object();
	require(parent != NULL, "large object fixture");
	for (int i = 0; i < 300; i++) {
		char key[16];
		snprintf(key, sizeof(key), "key-%03d", i);
		json_object_object_add(parent, key, json_object_new_int(i));
	}
	for (int i = 0; i < 300; i++) {
		char key[16];
		snprintf(key, sizeof(key), "key-%03d", i);
		require(json_object_get_int(json_object_object_get(parent, key)) == i,
			"geometric linkhash growth preserves every entry");
	}
	serialized = json_object_to_json_string(parent);
	require(serialized != NULL, "large object serializes after repeated growth");
	require_parseable(serialized);
	json_object_put(parent);
}

static void test_checked_producers(void)
{
	struct json_object *parent;
	struct json_object *failed_value;
	const char *serialized;

	parent = json_object_new_object();
	require(parent != NULL, "checked object parent allocation");
	inject_at(1);
	failed_value = json_object_new_string("required");
	stop_injection();
	require(failed_value == NULL, "required object child constructor fails");
	require(json_object_object_add_checked(parent, "required", failed_value) == -1,
		"checked object add rejects a failed constructor");
	require(json_object_to_json_string(parent) == NULL,
		"checked object add marks parent unserializable");
	json_object_put(parent);

	parent = json_object_new_array();
	require(parent != NULL, "checked array parent allocation");
	inject_at(1);
	failed_value = json_object_new_string("required");
	stop_injection();
	require(failed_value == NULL, "required array child constructor fails");
	require(json_object_array_add_checked(parent, failed_value) == -1,
		"checked array add rejects a failed constructor");
	require(json_object_to_json_string(parent) == NULL,
		"checked array add marks parent unserializable");
	json_object_put(parent);

	parent = json_object_new_array();
	require(parent != NULL, "checked array put parent allocation");
	require(json_object_array_put_idx_checked(parent, 0, NULL) == -1,
		"checked array put rejects a missing required value");
	require(json_object_to_json_string(parent) == NULL,
		"checked array put marks parent unserializable");
	json_object_put(parent);

	parent = json_object_new_object();
	require(parent != NULL, "literal null object fixture");
	json_object_object_add(parent, "explicit_null", NULL);
	serialized = json_object_to_json_string(parent);
	require(serialized != NULL, "legacy object add preserves JSON null support");
	require_parseable(serialized);
	json_object_put(parent);

	parent = json_object_new_array();
	require(parent != NULL, "literal null array fixture");
	require(json_object_array_add(parent, NULL) == 0,
		"legacy array add preserves JSON null support");
	serialized = json_object_to_json_string(parent);
	require(serialized != NULL, "literal null array serializes");
	require_parseable(serialized);
	json_object_put(parent);
}

static void test_tokener_reset_after_oom(void)
{
	struct json_tokener *tok = json_tokener_new();
	struct json_object *parsed;
	char input[256];
	require(tok != NULL, "reusable tokener allocation");
	input[0] = '"';
	memset(input + 1, 'x', sizeof(input) - 3);
	input[sizeof(input) - 2] = '"';
	input[sizeof(input) - 1] = '\0';
	inject_at(1);
	parsed = json_tokener_parse_ex(tok, input, -1);
	stop_injection();
	require(parsed == NULL && tok->err == json_tokener_error_memory,
		"long token reports printbuf allocation failure");
	json_tokener_reset(tok);
	parsed = json_tokener_parse_ex(tok, "[]", -1);
	require(parsed != NULL && tok->err == json_tokener_success,
		"tokener reset clears sticky printbuf failure");
	json_object_put(parsed);
	json_tokener_free(tok);
}

static struct json_object *parse_command(void)
{
	static const char command[] =
		"{\"action\":\"status.get\",\"params\":{"
		"\"ids\":[\"a\",\"b\",\"c\",\"d\"],"
		"\"limits\":{\"offset\":1,\"count\":8},\"enabled\":true}}";
	return json_tokener_parse_verbose(command, &parse_error);
}

static void test_parser_allocation_failures(void)
{
	struct json_object *parsed;
	size_t calls;
	stop_injection();
	parsed = parse_command();
	require(parsed != NULL && parse_error == json_tokener_success,
		"nested command parses before failure injection");
	calls = allocation_calls;
	require(calls > 10, "nested command exercises allocation paths");
	json_object_put(parsed);

	for (size_t nth = 1; nth <= calls; nth++) {
		inject_at(nth);
		parsed = parse_command();
		if (parsed) fprintf(stderr, "allocation ordinal %zu returned a JSON object (error=%d, calls=%zu)\n", nth, parse_error, allocation_calls);
		require(parsed == NULL, "allocation failure rejects incomplete parsed command");
		require(parse_error == json_tokener_error_memory,
			"allocation failure is reported as a parser memory error");
		stop_injection();
	}
	parsed = parse_command();
	require(parsed != NULL && parse_error == json_tokener_success,
		"nested command parsing succeeds after allocation failures");
	require(json_object_object_get(parsed, "params") != NULL,
		"successful parse retains the complete params object");
	json_object_put(parsed);
}

int main(void)
{
	test_constructor_failures(new_object, 3, "new_object handles table allocation failure");
	test_constructor_failures(new_array, 3, "new_array handles storage allocation failure");
	test_string_allocations();
	test_object_insert_failures();
	test_array_insert_failures();
	test_nested_error_and_growth();
	test_checked_producers();
	test_parser_allocation_failures();
	test_tokener_reset_after_oom();
	puts("json allocation failure, ownership, retry and growth checks passed");
	return 0;
}
