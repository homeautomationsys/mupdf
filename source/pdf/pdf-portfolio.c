#include "mupdf/fitz.h"
#include "mupdf/pdf.h"
#include "pdf-imp.h"

#include <string.h>

/*
	PDF Portfolio is just a sorted list of schema entries.
*/
struct pdf_portfolio_s
{
	pdf_obj *key;
	pdf_obj *val;
	int sort;
	pdf_portfolio_schema entry;
	pdf_portfolio *next;
};

static void
load_portfolio(fz_context *ctx, pdf_document *doc)
{
	pdf_obj *obj;
	int i, n;
	pdf_portfolio **pp;

	if (doc->portfolio)
		return;

	obj = pdf_dict_getl(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root), PDF_NAME(Collection), PDF_NAME(Schema), NULL);

	n = pdf_dict_len(ctx, obj);
	for (i = 0; i < n; i++)
	{
		pdf_obj *k = pdf_dict_get_key(ctx, obj, i);
		pdf_obj *v = pdf_dict_get_val(ctx, obj, i);
		int sort = pdf_dict_get_int(ctx, v, PDF_NAME(O));
		pdf_obj *eo = pdf_dict_get(ctx, v, PDF_NAME(E));
		int editable = eo ? pdf_to_bool(ctx, eo) : 0;
		pdf_obj *vo = pdf_dict_get(ctx, v, PDF_NAME(V));
		int visible = vo ? pdf_to_bool(ctx, vo) : 1;
		const char *subtype = pdf_to_name(ctx, pdf_dict_get(ctx, v, PDF_NAME(Subtype)));
		pdf_obj *name = pdf_dict_get(ctx, v, PDF_NAME(N));
		pdf_portfolio *p = fz_malloc_struct(ctx, pdf_portfolio);
		p->key = pdf_keep_obj(ctx, k);
		p->val = pdf_keep_obj(ctx, v);
		p->sort = sort;
		p->entry.visible = visible;
		p->entry.editable = editable;
		p->entry.name = pdf_keep_obj(ctx, name);
		if (!strcmp(subtype, "S"))
			p->entry.type = PDF_SCHEMA_TEXT;
		else if (!strcmp(subtype, "D"))
			p->entry.type = PDF_SCHEMA_DATE;
		else if (!strcmp(subtype, "N"))
			p->entry.type = PDF_SCHEMA_NUMBER;
		else if (!strcmp(subtype, "F"))
			p->entry.type = PDF_SCHEMA_FILENAME;
		else if (!strcmp(subtype, "Desc"))
			p->entry.type = PDF_SCHEMA_DESC;
		else if (!strcmp(subtype, "ModDate"))
			p->entry.type = PDF_SCHEMA_MODDATE;
		else if (!strcmp(subtype, "CreationDate"))
			p->entry.type = PDF_SCHEMA_CREATIONDATE;
		else if (!strcmp(subtype, "Size"))
			p->entry.type = PDF_SCHEMA_SIZE;
		else
			p->entry.type = PDF_SCHEMA_UNKNOWN;

		/* Now insert p */
		pp = &doc->portfolio;

		while (*pp && (*pp)->sort <= p->sort)
			pp = &(*pp)->next;

		p->next = *pp;
		*pp = p;
	}
}

int pdf_count_portfolio_schema(fz_context *ctx, pdf_document *doc)
{
	pdf_portfolio *port;
	int n;

	load_portfolio(ctx, doc);

	n = 0;
	for (port = doc->portfolio; port; port = port->next)
		++n;

	return n;
}

/*
	pdf_portfolio_schema_info: Fetch information about a given
	portfolio schema entry.

	doc: The document in question.

	entry: A value in the 0..n-1 range, where n is the
	value returned from pdf_count_portfolio_schema.

	info: Pointer to structure to fill in. Pointers within
	this structure may be set to NULL if no information is
	available.
*/
void pdf_portfolio_schema_info(fz_context *ctx, pdf_document *doc, int entry, pdf_portfolio_schema *info)
{
	pdf_portfolio *p;

	load_portfolio(ctx, doc);

	p = doc->portfolio;
	while (p && entry > 0)
		p = p->next, entry--;

	if (p == NULL || entry)
		fz_throw(ctx, FZ_ERROR_GENERIC, "entry out of range in pdf_portfolio_schema_info");

	*info = p->entry;
}

void pdf_reorder_portfolio_schema(fz_context *ctx, pdf_document *doc, int entry, int new_pos)
{
	pdf_portfolio **pp;
	pdf_portfolio *p;

	load_portfolio(ctx, doc);

	/* Take p out */
	pp = &doc->portfolio;
	while (*pp && entry > 0)
		pp = &(*pp)->next, entry--;
	p = *pp;
	if (p == NULL || entry)
		fz_throw(ctx, FZ_ERROR_GENERIC, "entry out of range in pdf_reorder_portfolio_schema");
	*pp = p->next;

	/* Put p back in */
	pp = &doc->portfolio;
	while (*pp && new_pos > 0)
		pp = &(*pp)->next, new_pos--;
	p->next = *pp;
	*pp = p;

	/* Rewrite the underlying orderings */
	for (p = doc->portfolio, entry = 0; p; p = p->next, entry++)
		pdf_dict_put_int(ctx, p->val, PDF_NAME(O), entry);
}

void pdf_rename_portfolio_schema(fz_context *ctx, pdf_document *doc, int entry, const char *name, int name_len)
{
	pdf_portfolio *p;
	pdf_obj *s;

	load_portfolio(ctx, doc);

	p = doc->portfolio;
	while (p && entry > 0)
		p = p->next, entry--;

	if (p == NULL || entry)
		fz_throw(ctx, FZ_ERROR_GENERIC, "entry out of range in pdf_rename_portfolio_schema");

	s = pdf_new_string(ctx, name, name_len);
	pdf_drop_obj(ctx, p->entry.name);
	p->entry.name = s;
	pdf_dict_put(ctx, p->val, PDF_NAME(N), s);
}

typedef int (pdf_name_tree_map_fn)(fz_context *ctx, pdf_obj *container, pdf_obj *key, pdf_obj *val, void *arg);

static int
do_name_tree_map(fz_context *ctx, pdf_obj *tree, pdf_name_tree_map_fn *fn, void *arg)
{
	int i;
	int n = 0;
	int m = 0;

	fz_var(n);
	fz_var(m);

	if (pdf_mark_obj(ctx, tree))
		fz_throw(ctx, FZ_ERROR_GENERIC, "Recursive name tree!");

	fz_try(ctx)
	{
		pdf_obj *arr = pdf_dict_get(ctx, tree, PDF_NAME(Kids));
		n = pdf_array_len(ctx, arr);

		for (i = n; i > 0;)
		{
			i--;
			if (do_name_tree_map(ctx, pdf_array_get(ctx, arr, i), fn, arg))
			{
				pdf_array_delete(ctx, arr, i);
				n--;
			}
		}

		arr = pdf_dict_get(ctx, tree, PDF_NAME(Names));
		m = pdf_array_len(ctx, arr);

		if (m & 1)
			fz_throw(ctx, FZ_ERROR_GENERIC, "Malformed Names array");

		for (i = m; i > 0;)
		{
			i -= 2;
			if (fn(ctx, tree, pdf_array_get(ctx, arr, i), pdf_array_get(ctx, arr, i+1), arg))
			{
				pdf_array_delete(ctx, arr, i+1);
				pdf_array_delete(ctx, arr, i);
				m -= 2;
			}
		}
	}
	fz_always(ctx)
		pdf_unmark_obj(ctx, tree);
	fz_catch(ctx)
		fz_rethrow(ctx);

	return n == 0 && m == 0;
}

void pdf_name_tree_map(fz_context *ctx, pdf_obj *tree, pdf_name_tree_map_fn *fn, void *arg)
{
	(void)do_name_tree_map(ctx, tree, fn, arg);
}

static int delete_from_node(fz_context *ctx, pdf_obj *container, pdf_obj *key, pdf_obj *val, void *arg)
{
	pdf_obj *delete_key = (pdf_obj *)arg;

	pdf_dict_del(ctx, pdf_dict_get(ctx, val, PDF_NAME(CI)), delete_key);

	return 0;
}

void pdf_delete_portfolio_schema(fz_context *ctx, pdf_document *doc, int entry)
{
	pdf_portfolio **pp;
	pdf_portfolio *p;
	pdf_obj *s;

	load_portfolio(ctx, doc);

	pp = &doc->portfolio;
	while (*pp && entry > 0)
		pp = &(*pp)->next, entry--;

	p = *pp;
	if (p == NULL || entry)
		fz_throw(ctx, FZ_ERROR_GENERIC, "entry out of range in pdf_delete_portfolio_schema");
	*pp = p->next;

	/* Delete the key from the schema */
	s = pdf_dict_getl(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root), PDF_NAME(Collection), PDF_NAME(Schema), NULL);
	pdf_dict_del(ctx, s, p->key);

	/* Delete this entry from all the collection entries */
	s = pdf_dict_getl(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root), PDF_NAME(Names), PDF_NAME(EmbeddedFiles), NULL);
	pdf_name_tree_map(ctx, s, delete_from_node, p->key);

	pdf_drop_obj(ctx, p->entry.name);
	pdf_drop_obj(ctx, p->key);
	pdf_drop_obj(ctx, p->val);
	fz_free(ctx, p);
}

void pdf_add_portfolio_schema(fz_context *ctx, pdf_document *doc, int entry, const pdf_portfolio_schema *info)
{
	pdf_portfolio **pp;
	pdf_portfolio *p;
	pdf_obj *s;
	pdf_obj *sc = NULL;
	int num;
	char str_name[32];
	pdf_obj *num_name = NULL;

	load_portfolio(ctx, doc);

	fz_var(num_name);
	fz_var(sc);

	pp = &doc->portfolio;
	while (*pp && entry > 0)
		pp = &(*pp)->next, entry--;

	fz_try(ctx)
	{
		/* Find a name for the new schema entry */
		num = 0;
		do
		{
			pdf_drop_obj(ctx, num_name);
			num_name = NULL;
			num++;
			fz_snprintf(str_name, sizeof str_name, "%d", num);
			num_name = pdf_new_name(ctx, str_name);
			p = doc->portfolio;
			for (p = doc->portfolio; p; p = p->next)
				if (pdf_name_eq(ctx, num_name, p->key))
					break;
		}
		while (p);

		sc = pdf_new_dict(ctx, doc, 4);
		pdf_dict_put_bool(ctx, sc, PDF_NAME(E), !!info->editable);
		pdf_dict_put_bool(ctx, sc, PDF_NAME(V), !!info->visible);
		pdf_dict_put_drop(ctx, sc, PDF_NAME(N), info->name);
		pdf_dict_put(ctx, sc, PDF_NAME(Subtype), PDF_NAME(S));

		/* Add to our linked list (in the correct sorted place) */
		p = fz_malloc_struct(ctx, pdf_portfolio);
		p->entry = *info;
		p->sort = 0; /* Will be rewritten in a mo */
		p->key = pdf_keep_obj(ctx, num_name);
		p->val = pdf_keep_obj(ctx, sc);
		p->next = *pp;
		*pp = p;

		/* Add the key to the schema */
		s = pdf_dict_getl(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root), PDF_NAME(Collection), PDF_NAME(Schema), NULL);
		pdf_dict_put(ctx, s, num_name, sc);

		/* Renumber the schema entries */
		for (num = 0, p = doc->portfolio; p; num++, p = p->next)
		{
			pdf_dict_put_int(ctx, p->val, PDF_NAME(O), num);
			p->sort = num;
		}
	}
	fz_always(ctx)
	{
		pdf_drop_obj(ctx, num_name);
		pdf_drop_obj(ctx, sc);
	}
	fz_catch(ctx)
		fz_rethrow(ctx);
}

static int count_nodes(fz_context *ctx, pdf_obj *container, pdf_obj *key, pdf_obj *val, void *arg)
{
	int *count = (int *)arg;

	*count += 1;

	return 0;
}

/*
	pdf_count_portfolio_entries: Get the number of portfolio entries
	in this document.

	doc: The document in question.
*/
int pdf_count_portfolio_entries(fz_context *ctx, pdf_document *doc)
{
	pdf_obj *s;
	int count;

	load_portfolio(ctx, doc);

	s = pdf_dict_getl(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root), PDF_NAME(Names), PDF_NAME(EmbeddedFiles), NULL);
	count = 0;
	pdf_name_tree_map(ctx, s, count_nodes, &count);

	return count;
}

struct find_data {
	pdf_obj *key;
	pdf_obj *val;
	int count;
};

static int find_entry(fz_context *ctx, pdf_obj *container, pdf_obj *key, pdf_obj *val, void *arg)
{
	struct find_data *data = (struct find_data *)arg;

	if (data->count == 0)
	{
		data->key = key;
		data->val = val;
	}
	data->count--;

	return 0;
}

/*
	pdf_portfolio_entry_info: Fetch information about a given
	portfolio entry.

	doc: The document in question.

	entry: A value in the 0..n-1 range, where n is the
	value returned from pdf_count_portfolio.

	Returns pdf_object representing this entry. This reference
	is borrowed, so call pdf_keep_obj on it if you wish to keep
	it.
*/
static pdf_obj *pdf_portfolio_entry_obj_name(fz_context *ctx, pdf_document *doc, int entry, pdf_obj **name)
{
	struct find_data data;
	pdf_obj *s;

	if (name)
		*name = NULL;

	load_portfolio(ctx, doc);

	s = pdf_dict_getl(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root), PDF_NAME(Names), PDF_NAME(EmbeddedFiles), NULL);
	data.count = entry;
	data.key = NULL;
	data.val = NULL;
	pdf_name_tree_map(ctx, s, find_entry, &data);

	if (name)
		*name = data.key;
	return data.val;
}

static pdf_obj *pdf_portfolio_entry_obj(fz_context *ctx, pdf_document *doc, int entry)
{
	pdf_obj *name;

	return pdf_portfolio_entry_obj_name(ctx, doc, entry, &name);
}

pdf_obj *pdf_portfolio_entry_name(fz_context *ctx, pdf_document *doc, int entry)
{
	pdf_obj *name;

	(void)pdf_portfolio_entry_obj_name(ctx, doc, entry, &name);
	return name;
}

fz_buffer *pdf_portfolio_entry(fz_context *ctx, pdf_document *doc, int entry)
{
	pdf_obj *obj = pdf_portfolio_entry_obj(ctx, doc, entry);

	return pdf_load_stream(ctx, pdf_dict_getl(ctx, obj, PDF_NAME(EF), PDF_NAME(F), NULL));
}

pdf_obj *pdf_portfolio_entry_info(fz_context *ctx, pdf_document *doc, int entry, int schema_entry)
{
	pdf_obj *obj = pdf_portfolio_entry_obj_name(ctx, doc, entry, NULL);
	pdf_portfolio *p;
	pdf_obj *lookup;
	int ef = 0;

	if (!obj)
		return NULL;

	for (p = doc->portfolio; p != NULL && schema_entry > 0; p = p->next, schema_entry--);

	if (schema_entry)
		fz_throw(ctx, FZ_ERROR_GENERIC, "schema_entry out of range");

	switch (p->entry.type)
	{
	default:
	case PDF_SCHEMA_TEXT:
	case PDF_SCHEMA_DATE:
	case PDF_SCHEMA_NUMBER:
		lookup = NULL;
		break;
	case PDF_SCHEMA_FILENAME:
		lookup = PDF_NAME(UF);
		break;
	case PDF_SCHEMA_DESC:
		lookup = PDF_NAME(Desc);
		break;
	case PDF_SCHEMA_MODDATE:
		lookup = PDF_NAME(ModDate);
		ef = 1;
		break;
	case PDF_SCHEMA_CREATIONDATE:
		lookup = PDF_NAME(CreationDate);
		ef = 1;
		break;
	case PDF_SCHEMA_SIZE:
		lookup = PDF_NAME(Size);
		ef = 1;
		break;
	}
	if (lookup)
	{
		pdf_obj *res;

		if (ef)
			obj = pdf_dict_getl(ctx, obj, PDF_NAME(EF), PDF_NAME(F), PDF_NAME(Params), NULL);
		res = pdf_dict_get(ctx, obj, lookup);
		if (res == NULL && pdf_name_eq(ctx, lookup, PDF_NAME(UF)))
			res = pdf_dict_get(ctx, obj, PDF_NAME(F));
		return res;
	}
	return pdf_dict_getl(ctx, obj, PDF_NAME(CI), p->key, NULL);
}

typedef struct
{
	pdf_obj *key;
	pdf_obj *found;
	int found_index;
	pdf_obj *last;
	int last_index;
	int entry;
} find_data;

static int
find_position(fz_context *ctx, pdf_obj *container, pdf_obj *key, pdf_obj *val, void *arg)
{
	find_data *data = (find_data *)arg;

	if (data->found)
		return 0;
	data->entry++;
	if (data->last != container)
	{
		data->last = container;
		data->last_index = 0;
	}
	else
		data->last_index++;
	if (pdf_objcmp(ctx, key, data->key) > 0)
	{
		data->found = container;
		data->found_index = data->last_index;
	}
	return 0;
}

static int
pdf_name_tree_insert(fz_context *ctx, pdf_document *doc, pdf_obj *tree, pdf_obj *key, pdf_obj *val)
{
	find_data data;
	pdf_obj *names, *limits, *limit0, *limit1;

	data.key = key;
	data.found = NULL;
	data.found_index = 0;
	data.last = NULL;
	data.last_index = 0;
	data.entry = 0;
	pdf_name_tree_map(ctx, tree, find_position, &data);

	if (!data.found)
	{
		data.found = data.last;
		data.found_index = data.last_index;
	}
	if (!data.found)
	{
		/* Completely empty name tree! */
		pdf_dict_put_array(ctx, tree, PDF_NAME(Names), 2);
		pdf_dict_put_array(ctx, tree, PDF_NAME(Limits), 2);
		data.found = tree;
		data.found_index = 0;
	}

	names = pdf_dict_get(ctx, data.found, PDF_NAME(Names));
	if (names == NULL)
		names = pdf_dict_put_array(ctx, data.found, PDF_NAME(Names), 2);
	pdf_array_insert(ctx, names, key, 2*data.found_index);
	pdf_array_insert(ctx, names, val, 2*data.found_index+1);

	limits = pdf_dict_get(ctx, data.found, PDF_NAME(Limits));
	if (limits == NULL)
		limits = pdf_dict_put_array(ctx, data.found, PDF_NAME(Limits), 2);
	limit0 = pdf_array_get(ctx, limits, 0);
	limit1 = pdf_array_get(ctx, limits, 1);
	if (!pdf_is_string(ctx, limit0) || data.found_index == 0)
		pdf_array_put(ctx, limits, 0, key);
	if (!pdf_is_string(ctx, limit1) || 2 * (data.found_index+1) == pdf_array_len(ctx, limits))
		pdf_array_put(ctx, limits, 1, key);

	return data.entry;
}

int pdf_add_portfolio_entry(fz_context *ctx, pdf_document *doc,
				const char *name,
				const char *desc,
				const char *filename,
				fz_buffer *buf)
{
	int len, entry = 0;
	pdf_obj *ef, *f, *params, *s;
	pdf_obj *key;
	pdf_obj *val = NULL;

	fz_var(val);

	load_portfolio(ctx, doc);

	/* Portfolios were introduced in PDF 1.7. */
	if (doc->version < 17)
		doc->version = 17;

	key = pdf_new_text_string(ctx, name);
	fz_try(ctx)
	{
		val = pdf_new_dict(ctx, doc, 6);
		pdf_dict_put_dict(ctx, val, PDF_NAME(CI), 4);
		ef = pdf_dict_put_dict(ctx, val, PDF_NAME(EF), 4);
		pdf_dict_put_string(ctx, val, PDF_NAME(F), filename, strlen(filename));
		pdf_dict_put_text_string(ctx, val, PDF_NAME(UF), filename);
		pdf_dict_put_text_string(ctx, val, PDF_NAME(Desc), desc);
		pdf_dict_put(ctx, val, PDF_NAME(Type), PDF_NAME(Filespec));
		pdf_dict_put_drop(ctx, ef, PDF_NAME(F), (f = pdf_add_stream(ctx, doc, buf, NULL, 0)));
		len = fz_buffer_storage(ctx, buf, NULL);
		pdf_dict_put_int(ctx, f, PDF_NAME(DL), len);
		pdf_dict_put_int(ctx, f, PDF_NAME(Length), len);
		params = pdf_dict_put_dict(ctx, f, PDF_NAME(Params), 4);
		pdf_dict_put_int(ctx, params, PDF_NAME(Size), len);

		s = pdf_dict_getl(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root), PDF_NAME(Collection), NULL);
		if (s == NULL)
		{
			s = pdf_new_dict(ctx, doc, 4);
			pdf_dict_putl_drop(ctx, pdf_trailer(ctx, doc), s, PDF_NAME(Root), PDF_NAME(Collection), NULL);
		}

		s = pdf_dict_getl(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root), PDF_NAME(Names), PDF_NAME(EmbeddedFiles), NULL);
		if (s == NULL)
		{
			s = pdf_new_dict(ctx, doc, 4);
			pdf_dict_putl_drop(ctx, pdf_trailer(ctx, doc), s, PDF_NAME(Root), PDF_NAME(Names), PDF_NAME(EmbeddedFiles), NULL);
		}
		entry = pdf_name_tree_insert(ctx, doc, s, key, val);
	}
	fz_always(ctx)
	{
		pdf_drop_obj(ctx, key);
		pdf_drop_obj(ctx, val);
	}
	fz_catch(ctx)
		fz_rethrow(ctx);

	return entry;
}

void pdf_set_portfolio_entry_info(fz_context *ctx, pdf_document *doc, int entry, int schema_entry, pdf_obj *data)
{
	pdf_portfolio *p;
	pdf_obj *obj, *lookup;
	int ef = 0;

	load_portfolio(ctx, doc);

	obj = pdf_portfolio_entry_obj_name(ctx, doc, entry, NULL);
	if (!obj)
		fz_throw(ctx, FZ_ERROR_GENERIC, "Can't set info on non existent portfolio entry");

	for (p = doc->portfolio; p != NULL && schema_entry > 0; p = p->next, schema_entry--);

	if (schema_entry)
		fz_throw(ctx, FZ_ERROR_GENERIC, "schema_entry out of range");

	switch (p->entry.type)
	{
	default:
	case PDF_SCHEMA_TEXT:
	case PDF_SCHEMA_DATE:
	case PDF_SCHEMA_NUMBER:
		lookup = NULL;
		break;
	case PDF_SCHEMA_FILENAME:
		lookup = PDF_NAME(UF);
		break;
	case PDF_SCHEMA_DESC:
		lookup = PDF_NAME(Desc);
		break;
	case PDF_SCHEMA_MODDATE:
		lookup = PDF_NAME(ModDate);
		ef = 1;
		break;
	case PDF_SCHEMA_CREATIONDATE:
		lookup = PDF_NAME(CreationDate);
		ef = 1;
		break;
	case PDF_SCHEMA_SIZE:
		fz_throw(ctx, FZ_ERROR_GENERIC, "Can't set size!");
		break;
	}
	if (lookup)
	{
		if (ef)
			obj = pdf_dict_getl(ctx, obj, PDF_NAME(EF), PDF_NAME(F), PDF_NAME(Params), NULL);
		pdf_dict_put(ctx, obj, lookup, data);
		if (pdf_name_eq(ctx, lookup, PDF_NAME(UF)))
			pdf_dict_put(ctx, obj, PDF_NAME(F), data);
		return;
	}
	pdf_dict_putl(ctx, obj, data, PDF_NAME(CI), p->key, NULL);
}

void pdf_drop_portfolio(fz_context *ctx, pdf_document *doc)
{
	if (!doc)
		return;

	while (doc->portfolio)
	{
		pdf_portfolio *p = doc->portfolio;
		doc->portfolio = p->next;

		pdf_drop_obj(ctx, p->entry.name);
		pdf_drop_obj(ctx, p->key);
		pdf_drop_obj(ctx, p->val);
		fz_free(ctx, p);
	}
}
