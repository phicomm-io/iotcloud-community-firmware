#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

static size_t realloc_calls;
static size_t fail_on_realloc;

void *__real_realloc(void *ptr, size_t size);

void *__wrap_realloc(void *ptr, size_t size)
{
	realloc_calls++;
	if (fail_on_realloc != 0 && realloc_calls == fail_on_realloc)
		return NULL;
	return __real_realloc(ptr, size);
}

static void require(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "json printbuf regression: %s\n", message);
		exit(1);
	}
}

static struct json_object *make_document(void)
{
	struct json_object *root = json_object_new_object();
	struct json_object *sections = json_object_new_array();
	char payload[193];

	require(root != NULL && sections != NULL, "allocate document root");
	memset(payload, 'x', sizeof(payload) - 1);
	payload[sizeof(payload) - 1] = '\0';

	for (int section = 0; section < 8; section++) {
		struct json_object *entry = json_object_new_object();
		struct json_object *values = json_object_new_array();
		require(entry != NULL && values != NULL, "allocate nested section");
		for (int item = 0; item < 8; item++) {
			struct json_object *value = json_object_new_string(payload);
			require(value != NULL, "allocate string value");
			require(json_object_array_add(values, value) == 0,
				"append nested string value");
		}
		json_object_object_add(entry, "values", values);
		json_object_object_add(entry, "section", json_object_new_int(section));
		require(json_object_array_add(sections, entry) == 0,
			"append nested section");
	}
	json_object_object_add(root, "sections", sections);
	json_object_object_add(root, "tail", json_object_new_string(payload));
	return root;
}

static void require_parseable(const char *text)
{
	enum json_tokener_error error = json_tokener_success;
	struct json_object *parsed = json_tokener_parse_verbose(text, &error);
	require(parsed != NULL && error == json_tokener_success,
		"serialized document must parse completely");
	json_object_put(parsed);
}

static char *serialize_baseline(struct json_object *root, size_t *expansions)
{
	const char *text;
	char *copy;

	realloc_calls = 0;
	fail_on_realloc = 0;
	text = json_object_to_json_string(root);
	require(text != NULL, "baseline cached serialization");
	*expansions = realloc_calls;
	require(*expansions > 1, "large nested document must expand printbuf repeatedly");
	require_parseable(text);
	copy = malloc(strlen(text) + 1);
	require(copy != NULL, "copy baseline serialization");
	strcpy(copy, text);
	return copy;
}

static void check_cached_failures(const char *expected, size_t expansions)
{
	for (size_t nth = 1; nth <= expansions; nth++) {
		struct json_object *root = make_document();
		const char *text;
		require(root != NULL, "allocate cached-failure document");
		realloc_calls = 0;
		fail_on_realloc = nth;
		text = json_object_to_json_string(root);
		require(text == NULL, "failed cached serialization must return NULL");
		require(realloc_calls == nth, "sticky failure must stop later realloc attempts");

		fail_on_realloc = 0;
		realloc_calls = 0;
		text = json_object_to_json_string(root);
		require(text != NULL && strcmp(text, expected) == 0,
			"cached object must serialize fully after a failed attempt");
		require_parseable(text);
		json_object_put(root);
	}
}

static void check_independent_buffer_failures(struct json_object *root,
					      const char *expected,
					      size_t expansions)
{
	for (size_t nth = 1; nth <= expansions; nth++) {
		struct printbuf *pb;

		realloc_calls = 0;
		fail_on_realloc = nth;
		pb = json_object_to_json_string_ex(root);
		require(pb == NULL, "failed independent serialization must return NULL");
		require(realloc_calls == nth, "independent serialization stopped at failed expansion");

		fail_on_realloc = 0;
		realloc_calls = 0;
		pb = json_object_to_json_string_ex(root);
		require(pb != NULL && strcmp(pb->buf, expected) == 0,
			"independent serialization must recover after allocation failure");
		require_parseable(pb->buf);
		printbuf_free(pb);
	}
}

int main(void)
{
	struct json_object *root = make_document();
	size_t expansions;
	char *expected = serialize_baseline(root, &expansions);

	check_cached_failures(expected, expansions);
	check_independent_buffer_failures(root, expected, expansions);

	free(expected);
	json_object_put(root);
	puts("json printbuf allocation failure and retry checks passed");
	return 0;
}
