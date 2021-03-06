/*
 * Tests exercising the ldb key value operations.
 *
 *  Copyright (C) Andrew Bartlett <abartlet@samba.org> 2018
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

/*
 * from cmocka.c:
 * These headers or their equivalents should be included prior to
 * including
 * this header file.
 *
 * #include <stdarg.h>
 * #include <stddef.h>
 * #include <setjmp.h>
 *
 * This allows test applications to use custom definitions of C standard
 * library functions and types.
 *
 */

/*
 * A KV module is expected to have the following behaviour
 *
 * - A transaction must be open to perform any read, write or delete operation
 * - Writes and Deletes should not be visible until a transaction is commited
 * - Nested transactions are not permitted
 * - transactions can be rolled back and commited.
 * - supports iteration over all records in the database
 * - supports the update_in_iterate operation allowing entries to be
 *   re-keyed.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <errno.h>
#include <unistd.h>
#include <talloc.h>

#define TEVENT_DEPRECATED 1
#include <tevent.h>

#include <ldb.h>
#include <ldb_module.h>
#include <ldb_private.h>
#include <string.h>
#include <ctype.h>

#include <sys/wait.h>

#include "ldb_tdb/ldb_tdb.h"


#define DEFAULT_BE  "tdb"

#ifndef TEST_BE
#define TEST_BE DEFAULT_BE
#endif /* TEST_BE */

#define NUM_RECS 1024


struct test_ctx {
	struct tevent_context *ev;
	struct ldb_context *ldb;

	const char *dbfile;
	const char *lockfile;   /* lockfile is separate */

	const char *dbpath;
};

static void unlink_old_db(struct test_ctx *test_ctx)
{
	int ret;

	errno = 0;
	ret = unlink(test_ctx->lockfile);
	if (ret == -1 && errno != ENOENT) {
		fail();
	}

	errno = 0;
	ret = unlink(test_ctx->dbfile);
	if (ret == -1 && errno != ENOENT) {
		fail();
	}
}

static int noconn_setup(void **state)
{
	struct test_ctx *test_ctx;

	test_ctx = talloc_zero(NULL, struct test_ctx);
	assert_non_null(test_ctx);

	test_ctx->ev = tevent_context_init(test_ctx);
	assert_non_null(test_ctx->ev);

	test_ctx->ldb = ldb_init(test_ctx, test_ctx->ev);
	assert_non_null(test_ctx->ldb);

	test_ctx->dbfile = talloc_strdup(test_ctx, "kvopstest.ldb");
	assert_non_null(test_ctx->dbfile);

	test_ctx->lockfile = talloc_asprintf(test_ctx, "%s-lock",
					     test_ctx->dbfile);
	assert_non_null(test_ctx->lockfile);

	test_ctx->dbpath = talloc_asprintf(test_ctx,
			TEST_BE"://%s", test_ctx->dbfile);
	assert_non_null(test_ctx->dbpath);

	unlink_old_db(test_ctx);
	*state = test_ctx;
	return 0;
}

static int noconn_teardown(void **state)
{
	struct test_ctx *test_ctx = talloc_get_type_abort(*state,
							  struct test_ctx);

	unlink_old_db(test_ctx);
	talloc_free(test_ctx);
	return 0;
}

static int setup(void **state)
{
	struct test_ctx *test_ctx;
	int ret;
	struct ldb_ldif *ldif;
	const char *index_ldif =		\
		"dn: @INDEXLIST\n"
		"@IDXGUID: objectUUID\n"
		"@IDX_DN_GUID: GUID\n"
		"\n";

	noconn_setup((void **) &test_ctx);

	ret = ldb_connect(test_ctx->ldb, test_ctx->dbpath, 0, NULL);
	assert_int_equal(ret, 0);

	while ((ldif = ldb_ldif_read_string(test_ctx->ldb, &index_ldif))) {
		ret = ldb_add(test_ctx->ldb, ldif->msg);
		assert_int_equal(ret, LDB_SUCCESS);
	}
	*state = test_ctx;
	return 0;
}

static int teardown(void **state)
{
	struct test_ctx *test_ctx = talloc_get_type_abort(*state,
							  struct test_ctx);
	noconn_teardown((void **) &test_ctx);
	return 0;
}

static struct ltdb_private *get_ltdb(struct ldb_context *ldb)
{
	void *data = NULL;
	struct ltdb_private *ltdb = NULL;

	data = ldb_module_get_private(ldb->modules);
	assert_non_null(data);

	ltdb = talloc_get_type(data, struct ltdb_private);
	assert_non_null(ltdb);

	return ltdb;
}

static int parse(struct ldb_val key,
		 struct ldb_val data,
		 void *private_data)
{
	struct ldb_val* read = private_data;

	/* Yes, we essentially leak this.  That is OK */
	read->data = talloc_size(talloc_autofree_context(),
				 data.length);
	assert_non_null(read->data);

	memcpy(read->data, data.data, data.length);
	read->length = data.length;
	return LDB_SUCCESS;
}

/*
 * Test that data can be written to the kv store and be read back.
 */
static void test_add_get(void **state)
{
	int ret;
	struct test_ctx *test_ctx = talloc_get_type_abort(*state,
							  struct test_ctx);
	struct ltdb_private *ltdb = get_ltdb(test_ctx->ldb);
	uint8_t key_val[] = "TheKey";
	struct ldb_val key = {
		.data   = key_val,
		.length = sizeof(key_val)
	};

	uint8_t value[] = "The record contents";
	struct ldb_val data = {
		.data    = value,
		.length = sizeof(value)
	};

	struct ldb_val read;

	int flags = 0;
	TALLOC_CTX *tmp_ctx;

	tmp_ctx = talloc_new(test_ctx);
	assert_non_null(tmp_ctx);

	/*
	 * Begin a transaction
	 */
	ret = ltdb->kv_ops->begin_write(ltdb);
	assert_int_equal(ret, 0);

	/*
	 * Write the record
	 */
	ret = ltdb->kv_ops->store(ltdb, key, data, flags);
	assert_int_equal(ret, 0);

	/*
	 * Commit the transaction
	 */
	ret = ltdb->kv_ops->finish_write(ltdb);
	assert_int_equal(ret, 0);

	/*
	 * And now read it back
	 */
	ret = ltdb->kv_ops->lock_read(test_ctx->ldb->modules);
	assert_int_equal(ret, 0);

	ret = ltdb->kv_ops->fetch_and_parse(ltdb, key, parse, &read);
	assert_int_equal(ret, 0);

	assert_int_equal(sizeof(value), read.length);
	assert_memory_equal(value, read.data, sizeof(value));

	ret = ltdb->kv_ops->unlock_read(test_ctx->ldb->modules);
	assert_int_equal(ret, 0);
	talloc_free(tmp_ctx);
}

/*
 * Test that data can be deleted from the kv store
 */
static void test_delete(void **state)
{
	int ret;
	struct test_ctx *test_ctx = talloc_get_type_abort(*state,
							  struct test_ctx);
	struct ltdb_private *ltdb = get_ltdb(test_ctx->ldb);
	uint8_t key_val[] = "TheKey";
	struct ldb_val key = {
		.data   = key_val,
		.length = sizeof(key_val)
	};

	uint8_t value[] = "The record contents";
	struct ldb_val data = {
		.data    = value,
		.length = sizeof(value)
	};

	struct ldb_val read;

	int flags = 0;
	TALLOC_CTX *tmp_ctx;

	tmp_ctx = talloc_new(test_ctx);
	assert_non_null(tmp_ctx);

	/*
	 * Begin a transaction
	 */
	ret = ltdb->kv_ops->begin_write(ltdb);
	assert_int_equal(ret, 0);

	/*
	 * Write the record
	 */
	ret = ltdb->kv_ops->store(ltdb, key, data, flags);
	assert_int_equal(ret, 0);

	/*
	 * Commit the transaction
	 */
	ret = ltdb->kv_ops->finish_write(ltdb);
	assert_int_equal(ret, 0);

	/*
	 * And now read it back
	 */
	ret = ltdb->kv_ops->lock_read(test_ctx->ldb->modules);
	assert_int_equal(ret, 0);
	ret = ltdb->kv_ops->fetch_and_parse(ltdb, key, parse, &read);
	assert_int_equal(ret, 0);
	assert_int_equal(sizeof(value), read.length);
	assert_memory_equal(value, read.data, sizeof(value));
	ret = ltdb->kv_ops->unlock_read(test_ctx->ldb->modules);
	assert_int_equal(ret, 0);

	/*
	 * Begin a transaction
	 */
	ret = ltdb->kv_ops->begin_write(ltdb);
	assert_int_equal(ret, 0);

	/*
	 * Now delete it.
	 */
	ret = ltdb->kv_ops->delete(ltdb, key);
	assert_int_equal(ret, 0);

	/*
	 * Commit the transaction
	 */
	ret = ltdb->kv_ops->finish_write(ltdb);
	assert_int_equal(ret, 0);

	/*
	 * And now try to read it back
	 */
	ret = ltdb->kv_ops->lock_read(test_ctx->ldb->modules);
	assert_int_equal(ret, 0);
	ret = ltdb->kv_ops->fetch_and_parse(ltdb, key, parse, &read);
	assert_int_equal(ret, LDB_ERR_NO_SUCH_OBJECT);
	ret = ltdb->kv_ops->unlock_read(test_ctx->ldb->modules);
	assert_int_equal(ret, 0);

	talloc_free(tmp_ctx);
}

/*
 * Check that writes are correctly rolled back when a transaction
 * is rolled back.
 */
static void test_transaction_abort_write(void **state)
{
	int ret;
	struct test_ctx *test_ctx = talloc_get_type_abort(*state,
							  struct test_ctx);
	struct ltdb_private *ltdb = get_ltdb(test_ctx->ldb);
	uint8_t key_val[] = "TheKey";
	struct ldb_val key = {
		.data   = key_val,
		.length = sizeof(key_val)
	};

	uint8_t value[] = "The record contents";
	struct ldb_val data = {
		.data    = value,
		.length = sizeof(value)
	};

	struct ldb_val read;

	int flags = 0;
	TALLOC_CTX *tmp_ctx;

	tmp_ctx = talloc_new(test_ctx);
	assert_non_null(tmp_ctx);

	/*
	 * Begin a transaction
	 */
	ret = ltdb->kv_ops->begin_write(ltdb);
	assert_int_equal(ret, 0);

	/*
	 * Write the record
	 */
	ret = ltdb->kv_ops->store(ltdb, key, data, flags);
	assert_int_equal(ret, 0);

	/*
	 * And now read it back
	 */
	ret = ltdb->kv_ops->fetch_and_parse(ltdb, key, parse, &read);
	assert_int_equal(ret, 0);
	assert_int_equal(sizeof(value), read.length);
	assert_memory_equal(value, read.data, sizeof(value));


	/*
	 * Now abort the transaction
	 */
	ret = ltdb->kv_ops->abort_write(ltdb);
	assert_int_equal(ret, 0);

	/*
	 * And now read it back, should not be there
	 */
	ret = ltdb->kv_ops->lock_read(test_ctx->ldb->modules);
	assert_int_equal(ret, 0);
	ret = ltdb->kv_ops->fetch_and_parse(ltdb, key, parse, &read);
	assert_int_equal(ret, LDB_ERR_NO_SUCH_OBJECT);
	ret = ltdb->kv_ops->unlock_read(test_ctx->ldb->modules);
	assert_int_equal(ret, 0);

	talloc_free(tmp_ctx);
}

/*
 * Check that deletes are correctly rolled back when a transaction is
 * aborted.
 */
static void test_transaction_abort_delete(void **state)
{
	int ret;
	struct test_ctx *test_ctx = talloc_get_type_abort(*state,
							  struct test_ctx);
	struct ltdb_private *ltdb = get_ltdb(test_ctx->ldb);
	uint8_t key_val[] = "TheKey";
	struct ldb_val key = {
		.data   = key_val,
		.length = sizeof(key_val)
	};

	uint8_t value[] = "The record contents";
	struct ldb_val data = {
		.data    = value,
		.length = sizeof(value)
	};

	struct ldb_val read;

	int flags = 0;
	TALLOC_CTX *tmp_ctx;

	tmp_ctx = talloc_new(test_ctx);
	assert_non_null(tmp_ctx);

	/*
	 * Begin a transaction
	 */
	ret = ltdb->kv_ops->begin_write(ltdb);
	assert_int_equal(ret, 0);

	/*
	 * Write the record
	 */
	ret = ltdb->kv_ops->store(ltdb, key, data, flags);
	assert_int_equal(ret, 0);

	/*
	 * Commit the transaction
	 */
	ret = ltdb->kv_ops->finish_write(ltdb);
	assert_int_equal(ret, 0);

	/*
	 * And now read it back
	 */
	ret = ltdb->kv_ops->lock_read(test_ctx->ldb->modules);
	assert_int_equal(ret, 0);
	ret = ltdb->kv_ops->fetch_and_parse(ltdb, key, parse, &read);
	assert_int_equal(ret, 0);
	assert_int_equal(sizeof(value), read.length);
	assert_memory_equal(value, read.data, sizeof(value));
	ret = ltdb->kv_ops->unlock_read(test_ctx->ldb->modules);
	assert_int_equal(ret, 0);

	/*
	 * Begin a transaction
	 */
	ret = ltdb->kv_ops->begin_write(ltdb);
	assert_int_equal(ret, 0);

	/*
	 * Now delete it.
	 */
	ret = ltdb->kv_ops->delete(ltdb, key);
	assert_int_equal(ret, 0);

	/*
	 * And now read it back
	 */
	ret = ltdb->kv_ops->fetch_and_parse(ltdb, key, parse, &read);
	assert_int_equal(ret, LDB_ERR_NO_SUCH_OBJECT);

	/*
	 * Abort the transaction
	 */
	ret = ltdb->kv_ops->abort_write(ltdb);
	assert_int_equal(ret, 0);

	/*
	 * And now try to read it back
	 */
	ret = ltdb->kv_ops->lock_read(test_ctx->ldb->modules);
	assert_int_equal(ret, 0);
	ret = ltdb->kv_ops->fetch_and_parse(ltdb, key, parse, &read);
	assert_int_equal(ret, 0);
	assert_int_equal(sizeof(value), read.length);
	assert_memory_equal(value, read.data, sizeof(value));
	ret = ltdb->kv_ops->unlock_read(test_ctx->ldb->modules);
	assert_int_equal(ret, 0);

	talloc_free(tmp_ctx);
}

/*
 * Test that writes outside a transaction fail
 */
static void test_write_outside_transaction(void **state)
{
	int ret;
	struct test_ctx *test_ctx = talloc_get_type_abort(*state,
							  struct test_ctx);
	struct ltdb_private *ltdb = get_ltdb(test_ctx->ldb);
	uint8_t key_val[] = "TheKey";
	struct ldb_val key = {
		.data   = key_val,
		.length = sizeof(key_val)
	};

	uint8_t value[] = "The record contents";
	struct ldb_val data = {
		.data    = value,
		.length = sizeof(value)
	};


	int flags = 0;
	TALLOC_CTX *tmp_ctx;

	tmp_ctx = talloc_new(test_ctx);
	assert_non_null(tmp_ctx);

	/*
	 * Attempt to write the record
	 */
	ret = ltdb->kv_ops->store(ltdb, key, data, flags);
	assert_int_equal(ret, LDB_ERR_PROTOCOL_ERROR);

	talloc_free(tmp_ctx);
}

/*
 * Test data can not be deleted outside a transaction
 */
static void test_delete_outside_transaction(void **state)
{
	int ret;
	struct test_ctx *test_ctx = talloc_get_type_abort(*state,
							  struct test_ctx);
	struct ltdb_private *ltdb = get_ltdb(test_ctx->ldb);
	uint8_t key_val[] = "TheKey";
	struct ldb_val key = {
		.data   = key_val,
		.length = sizeof(key_val)
	};

	uint8_t value[] = "The record contents";
	struct ldb_val data = {
		.data    = value,
		.length = sizeof(value)
	};

	struct ldb_val read;

	int flags = 0;
	TALLOC_CTX *tmp_ctx;

	tmp_ctx = talloc_new(test_ctx);
	assert_non_null(tmp_ctx);

	/*
	 * Begin a transaction
	 */
	ret = ltdb->kv_ops->begin_write(ltdb);
	assert_int_equal(ret, 0);

	/*
	 * Write the record
	 */
	ret = ltdb->kv_ops->store(ltdb, key, data, flags);
	assert_int_equal(ret, 0);

	/*
	 * Commit the transaction
	 */
	ret = ltdb->kv_ops->finish_write(ltdb);
	assert_int_equal(ret, 0);

	/*
	 * And now read it back
	 */
	ret = ltdb->kv_ops->lock_read(test_ctx->ldb->modules);
	assert_int_equal(ret, 0);
	ret = ltdb->kv_ops->fetch_and_parse(ltdb, key, parse, &read);
	assert_int_equal(ret, 0);
	assert_int_equal(sizeof(value), read.length);
	assert_memory_equal(value, read.data, sizeof(value));
	ret = ltdb->kv_ops->unlock_read(test_ctx->ldb->modules);
	assert_int_equal(ret, 0);

	/*
	 * Now attempt to delete a record
	 */
	ret = ltdb->kv_ops->delete(ltdb, key);
	assert_int_equal(ret, LDB_ERR_PROTOCOL_ERROR);

	/*
	 * And now read it back
	 */
	ret = ltdb->kv_ops->lock_read(test_ctx->ldb->modules);
	assert_int_equal(ret, 0);
	ret = ltdb->kv_ops->fetch_and_parse(ltdb, key, parse, &read);
	assert_int_equal(ret, 0);
	assert_int_equal(sizeof(value), read.length);
	assert_memory_equal(value, read.data, sizeof(value));
	ret = ltdb->kv_ops->unlock_read(test_ctx->ldb->modules);
	assert_int_equal(ret, 0);

	talloc_free(tmp_ctx);
}

static int traverse_fn(struct ltdb_private *ltdb,
		       struct ldb_val key,
		       struct ldb_val data,
		       void *ctx) {

	int *visits = ctx;
	int i;

	if (strncmp("key ", (char *) key.data, 4) == 0) {
		i = strtol((char *) &key.data[4], NULL, 10);
		visits[i]++;
	}
	return LDB_SUCCESS;
}


/*
 * Test that iterate visits all the records.
 */
static void test_iterate(void **state)
{
	int ret;
	struct test_ctx *test_ctx = talloc_get_type_abort(*state,
							  struct test_ctx);
	struct ltdb_private *ltdb = get_ltdb(test_ctx->ldb);
	int i;
	int num_recs = 1024;
	int visits[num_recs];

	TALLOC_CTX *tmp_ctx;

	tmp_ctx = talloc_new(test_ctx);
	assert_non_null(tmp_ctx);

	/*
	 * Begin a transaction
	 */
	ret = ltdb->kv_ops->begin_write(ltdb);
	assert_int_equal(ret, 0);

	/*
	 * Write the records
	 */
	for (i = 0; i < num_recs; i++) {
		struct ldb_val key;
		struct ldb_val rec;
		int flags = 0;

		visits[i] = 0;
		key.data   = (uint8_t *)talloc_asprintf(tmp_ctx, "key %04d", i);
		key.length = strlen((char *)key.data) + 1;

		rec.data = (uint8_t *) talloc_asprintf(tmp_ctx,
						       "data for record (%04d)",
						       i);
		rec.length = strlen((char *)rec.data) + 1;

		ret = ltdb->kv_ops->store(ltdb, key, rec, flags);
		assert_int_equal(ret, 0);

		TALLOC_FREE(key.data);
		TALLOC_FREE(rec.data);
	}

	/*
	 * Commit the transaction
	 */
	ret = ltdb->kv_ops->finish_write(ltdb);
	assert_int_equal(ret, 0);

	/*
	 * Now iterate over the kv store and ensure that all the
	 * records are visited.
	 */
	ret = ltdb->kv_ops->lock_read(test_ctx->ldb->modules);
	assert_int_equal(ret, 0);
	ret = ltdb->kv_ops->iterate(ltdb, traverse_fn, visits);
	for (i = 0; i <num_recs; i++) {
		assert_int_equal(1, visits[i]);
	}
	ret = ltdb->kv_ops->unlock_read(test_ctx->ldb->modules);
	assert_int_equal(ret, 0);

	TALLOC_FREE(tmp_ctx);
}

/*
 * Ensure that writes are not visible until the transaction has been
 * committed.
 */
static void test_write_transaction_isolation(void **state)
{
	int ret;
	struct test_ctx *test_ctx = talloc_get_type_abort(*state,
							  struct test_ctx);
	struct ltdb_private *ltdb = get_ltdb(test_ctx->ldb);
	struct ldb_val key;
	struct ldb_val val;

	const char *KEY1 = "KEY01";
	const char *VAL1 = "VALUE01";

	const char *KEY2 = "KEY02";
	const char *VAL2 = "VALUE02";

	/*
	 * Pipes etc to co-ordinate the processes
	 */
	int to_child[2];
	int to_parent[2];
	char buf[2];
	pid_t pid, w_pid;
	int wstatus;

	TALLOC_CTX *tmp_ctx;
	tmp_ctx = talloc_new(test_ctx);
	assert_non_null(tmp_ctx);


	/*
	 * Add a record to the database
	 */
	ret = ltdb->kv_ops->begin_write(ltdb);
	assert_int_equal(ret, 0);

	key.data = (uint8_t *)talloc_strdup(tmp_ctx, KEY1);
	key.length = strlen(KEY1) + 1;

	val.data = (uint8_t *)talloc_strdup(tmp_ctx, VAL1);
	val.length = strlen(VAL1) + 1;

	ret = ltdb->kv_ops->store(ltdb, key, val, 0);
	assert_int_equal(ret, 0);

	ret = ltdb->kv_ops->finish_write(ltdb);
	assert_int_equal(ret, 0);


	ret = pipe(to_child);
	assert_int_equal(ret, 0);
	ret = pipe(to_parent);
	assert_int_equal(ret, 0);
	/*
	 * Now fork a new process
	 */

	pid = fork();
	if (pid == 0) {

		struct ldb_context *ldb = NULL;
		close(to_child[1]);
		close(to_parent[0]);

		/*
		 * Wait for the transaction to start
		 */
		ret = read(to_child[0], buf, 2);
		if (ret != 2) {
			print_error(__location__": read returned (%d)\n",
				    ret);
			exit(LDB_ERR_OPERATIONS_ERROR);
		}
		ldb = ldb_init(test_ctx, test_ctx->ev);
		ret = ldb_connect(ldb, test_ctx->dbpath, 0, NULL);
		if (ret != LDB_SUCCESS) {
			print_error(__location__": ldb_connect returned (%d)\n",
				    ret);
			exit(ret);
		}

		ltdb = get_ltdb(ldb);

		ret = ltdb->kv_ops->lock_read(ldb->modules);
		if (ret != LDB_SUCCESS) {
			print_error(__location__": lock_read returned (%d)\n",
				    ret);
			exit(ret);
		}

		/*
		 * Check that KEY1 is there
		 */
		key.data = (uint8_t *)talloc_strdup(tmp_ctx, KEY1);
		key.length = strlen(KEY1) + 1;

		ret = ltdb->kv_ops->fetch_and_parse(ltdb, key, parse, &val);
		if (ret != LDB_SUCCESS) {
			print_error(__location__": fetch_and_parse returned "
				    "(%d)\n",
				    ret);
			exit(ret);
		}

		if ((strlen(VAL1) + 1) != val.length) {
			print_error(__location__": KEY1 value lengths different"
				    ", expected (%d) actual(%d)\n",
				    (int)(strlen(VAL1) + 1), (int)val.length);
			exit(LDB_ERR_OPERATIONS_ERROR);
		}
		if (memcmp(VAL1, val.data, strlen(VAL1)) != 0) {
			print_error(__location__": KEY1 values different, "
				    "expected (%s) actual(%s)\n",
				    VAL1,
				    val.data);
			exit(LDB_ERR_OPERATIONS_ERROR);
		}

		ret = ltdb->kv_ops->unlock_read(ldb->modules);
		if (ret != LDB_SUCCESS) {
			print_error(__location__": unlock_read returned (%d)\n",
				    ret);
			exit(ret);
		}

		/*
		 * Check that KEY2 is not there
		 */
		key.data = (uint8_t *)talloc_strdup(tmp_ctx, KEY2);
		key.length = strlen(KEY2 + 1);

		ret = ltdb->kv_ops->lock_read(ldb->modules);
		if (ret != LDB_SUCCESS) {
			print_error(__location__": lock_read returned (%d)\n",
				    ret);
			exit(ret);
		}

		ret = ltdb->kv_ops->fetch_and_parse(ltdb, key, parse, &val);
		if (ret != LDB_ERR_NO_SUCH_OBJECT) {
			print_error(__location__": fetch_and_parse returned "
				    "(%d)\n",
				    ret);
			exit(ret);
		}

		ret = ltdb->kv_ops->unlock_read(ldb->modules);
		if (ret != LDB_SUCCESS) {
			print_error(__location__": unlock_read returned (%d)\n",
				    ret);
			exit(ret);
		}

		/*
		 * Signal the other process to commit the transaction
		 */
		ret = write(to_parent[1], "GO", 2);
		if (ret != 2) {
			print_error(__location__": write returned (%d)\n",
				    ret);
			exit(LDB_ERR_OPERATIONS_ERROR);
		}

		/*
		 * Wait for the transaction to be commited
		 */
		ret = read(to_child[0], buf, 2);
		if (ret != 2) {
			print_error(__location__": read returned (%d)\n",
				    ret);
			exit(LDB_ERR_OPERATIONS_ERROR);
		}

		/*
		 * Check that KEY1 is there
		 */
		ret = ltdb->kv_ops->lock_read(ldb->modules);
		if (ret != LDB_SUCCESS) {
			print_error(__location__": unlock_read returned (%d)\n",
				    ret);
			exit(ret);
		}
		key.data = (uint8_t *)talloc_strdup(tmp_ctx, KEY1);
		key.length = strlen(KEY1) + 1;

		ret = ltdb->kv_ops->fetch_and_parse(ltdb, key, parse, &val);
		if (ret != LDB_SUCCESS) {
			print_error(__location__": fetch_and_parse returned "
				    "(%d)\n",
				    ret);
			exit(ret);
		}

		if ((strlen(VAL1) + 1) != val.length) {
			print_error(__location__": KEY1 value lengths different"
				    ", expected (%d) actual(%d)\n",
				    (int)(strlen(VAL1) + 1), (int)val.length);
			exit(LDB_ERR_OPERATIONS_ERROR);
		}
		if (memcmp(VAL1, val.data, strlen(VAL1)) != 0) {
			print_error(__location__": KEY1 values different, "
				    "expected (%s) actual(%s)\n",
				    VAL1,
				    val.data);
			exit(LDB_ERR_OPERATIONS_ERROR);
		}

		ret = ltdb->kv_ops->unlock_read(ldb->modules);
		if (ret != LDB_SUCCESS) {
			print_error(__location__": unlock_read returned (%d)\n",
				    ret);
			exit(ret);
		}


		/*
		 * Check that KEY2 is there
		 */
		ret = ltdb->kv_ops->lock_read(ldb->modules);
		if (ret != LDB_SUCCESS) {
			print_error(__location__": unlock_read returned (%d)\n",
				    ret);
			exit(ret);
		}

		key.data = (uint8_t *)talloc_strdup(tmp_ctx, KEY2);
		key.length = strlen(KEY2) + 1;

		ret = ltdb->kv_ops->fetch_and_parse(ltdb, key, parse, &val);
		if (ret != LDB_SUCCESS) {
			print_error(__location__": fetch_and_parse returned "
				    "(%d)\n",
				    ret);
			exit(ret);
		}

		if ((strlen(VAL2) + 1) != val.length) {
			print_error(__location__": KEY2 value lengths different"
				    ", expected (%d) actual(%d)\n",
				    (int)(strlen(VAL2) + 1), (int)val.length);
			exit(LDB_ERR_OPERATIONS_ERROR);
		}
		if (memcmp(VAL2, val.data, strlen(VAL2)) != 0) {
			print_error(__location__": KEY2 values different, "
				    "expected (%s) actual(%s)\n",
				    VAL2,
				    val.data);
			exit(LDB_ERR_OPERATIONS_ERROR);
		}

		ret = ltdb->kv_ops->unlock_read(ldb->modules);
		if (ret != LDB_SUCCESS) {
			print_error(__location__": unlock_read returned (%d)\n",
				    ret);
			exit(ret);
		}

		exit(0);
	}
	close(to_child[0]);
	close(to_parent[1]);

	/*
	 * Begin a transaction and add a record to the database
	 * but leave the transaction open
	 */
	ret = ltdb->kv_ops->begin_write(ltdb);
	assert_int_equal(ret, 0);

	key.data = (uint8_t *)talloc_strdup(tmp_ctx, KEY2);
	key.length = strlen(KEY2) + 1;

	val.data = (uint8_t *)talloc_strdup(tmp_ctx, VAL2);
	val.length = strlen(VAL2) + 1;

	ret = ltdb->kv_ops->store(ltdb, key, val, 0);
	assert_int_equal(ret, 0);

	/*
	 * Signal the child process
	 */
	ret = write(to_child[1], "GO", 2);
	assert_int_equal(2, ret);

	/*
	 * Wait for the child process to check the DB state while the
	 * transaction is active
	 */
	ret = read(to_parent[0], buf, 2);
	assert_int_equal(2, ret);

	/*
	 * commit the transaction
	 */
	ret = ltdb->kv_ops->finish_write(ltdb);
	assert_int_equal(0, ret);

	/*
	 * Signal the child process
	 */
	ret = write(to_child[1], "GO", 2);
	assert_int_equal(2, ret);

	w_pid = waitpid(pid, &wstatus, 0);
	assert_int_equal(pid, w_pid);

	assert_true(WIFEXITED(wstatus));

	assert_int_equal(WEXITSTATUS(wstatus), 0);


	TALLOC_FREE(tmp_ctx);
}

/*
 * Ensure that deletes are not visible until the transaction has been
 * committed.
 */
static void test_delete_transaction_isolation(void **state)
{
	int ret;
	struct test_ctx *test_ctx = talloc_get_type_abort(*state,
							  struct test_ctx);
	struct ltdb_private *ltdb = get_ltdb(test_ctx->ldb);
	struct ldb_val key;
	struct ldb_val val;

	const char *KEY1 = "KEY01";
	const char *VAL1 = "VALUE01";

	const char *KEY2 = "KEY02";
	const char *VAL2 = "VALUE02";

	/*
	 * Pipes etc to co-ordinate the processes
	 */
	int to_child[2];
	int to_parent[2];
	char buf[2];
	pid_t pid, w_pid;
	int wstatus;

	TALLOC_CTX *tmp_ctx;
	tmp_ctx = talloc_new(test_ctx);
	assert_non_null(tmp_ctx);


	/*
	 * Add records to the database
	 */
	ret = ltdb->kv_ops->begin_write(ltdb);
	assert_int_equal(ret, 0);

	key.data = (uint8_t *)talloc_strdup(tmp_ctx, KEY1);
	key.length = strlen(KEY1) + 1;

	val.data = (uint8_t *)talloc_strdup(tmp_ctx, VAL1);
	val.length = strlen(VAL1) + 1;

	ret = ltdb->kv_ops->store(ltdb, key, val, 0);
	assert_int_equal(ret, 0);

	key.data = (uint8_t *)talloc_strdup(tmp_ctx, KEY2);
	key.length = strlen(KEY2) + 1;

	val.data = (uint8_t *)talloc_strdup(tmp_ctx, VAL2);
	val.length = strlen(VAL2) + 1;

	ret = ltdb->kv_ops->store(ltdb, key, val, 0);
	assert_int_equal(ret, 0);

	ret = ltdb->kv_ops->finish_write(ltdb);
	assert_int_equal(ret, 0);


	ret = pipe(to_child);
	assert_int_equal(ret, 0);
	ret = pipe(to_parent);
	assert_int_equal(ret, 0);
	/*
	 * Now fork a new process
	 */

	pid = fork();
	if (pid == 0) {

		struct ldb_context *ldb = NULL;
		close(to_child[1]);
		close(to_parent[0]);

		/*
		 * Wait for the transaction to be started
		 */
		ret = read(to_child[0], buf, 2);
		if (ret != 2) {
			print_error(__location__": read returned (%d)\n",
				    ret);
			exit(LDB_ERR_OPERATIONS_ERROR);
		}

		ldb = ldb_init(test_ctx, test_ctx->ev);
		ret = ldb_connect(ldb, test_ctx->dbpath, 0, NULL);
		if (ret != LDB_SUCCESS) {
			print_error(__location__": ldb_connect returned (%d)\n",
				    ret);
			exit(ret);
		}

		ltdb = get_ltdb(ldb);

		ret = ltdb->kv_ops->lock_read(ldb->modules);
		if (ret != LDB_SUCCESS) {
			print_error(__location__": lock_read returned (%d)\n",
				    ret);
			exit(ret);
		}

		/*
		 * Check that KEY1 is there
		 */
		key.data = (uint8_t *)talloc_strdup(tmp_ctx, KEY1);
		key.length = strlen(KEY1) + 1;

		ret = ltdb->kv_ops->fetch_and_parse(ltdb, key, parse, &val);
		if (ret != LDB_SUCCESS) {
			print_error(__location__": fetch_and_parse returned "
				    "(%d)\n",
				    ret);
			exit(ret);
		}

		if ((strlen(VAL1) + 1) != val.length) {
			print_error(__location__": KEY1 value lengths different"
				    ", expected (%d) actual(%d)\n",
				    (int)(strlen(VAL1) + 1), (int)val.length);
			exit(LDB_ERR_OPERATIONS_ERROR);
		}
		if (memcmp(VAL1, val.data, strlen(VAL1)) != 0) {
			print_error(__location__": KEY1 values different, "
				    "expected (%s) actual(%s)\n",
				    VAL1,
				    val.data);
			exit(LDB_ERR_OPERATIONS_ERROR);
		}

		/*
		 * Check that KEY2 is there
		 */

		key.data = (uint8_t *)talloc_strdup(tmp_ctx, KEY2);
		key.length = strlen(KEY2) + 1;

		ret = ltdb->kv_ops->fetch_and_parse(ltdb, key, parse, &val);
		if (ret != LDB_SUCCESS) {
			print_error(__location__": fetch_and_parse returned "
				    "(%d)\n",
				    ret);
			exit(ret);
		}

		if ((strlen(VAL2) + 1) != val.length) {
			print_error(__location__": KEY2 value lengths different"
				    ", expected (%d) actual(%d)\n",
				    (int)(strlen(VAL2) + 1), (int)val.length);
			exit(LDB_ERR_OPERATIONS_ERROR);
		}
		if (memcmp(VAL2, val.data, strlen(VAL2)) != 0) {
			print_error(__location__": KEY2 values different, "
				    "expected (%s) actual(%s)\n",
				    VAL2,
				    val.data);
			exit(LDB_ERR_OPERATIONS_ERROR);
		}

		ret = ltdb->kv_ops->unlock_read(ldb->modules);
		if (ret != LDB_SUCCESS) {
			print_error(__location__": unlock_read returned (%d)\n",
				    ret);
			exit(ret);
		}

		/*
		 * Signal the other process to commit the transaction
		 */
		ret = write(to_parent[1], "GO", 2);
		if (ret != 2) {
			print_error(__location__": write returned (%d)\n",
				    ret);
			exit(LDB_ERR_OPERATIONS_ERROR);
		}

		/*
		 * Wait for the transaction to be commited
		 */
		ret = read(to_child[0], buf, 2);
		if (ret != 2) {
			print_error(__location__": read returned (%d)\n",
				    ret);
			exit(LDB_ERR_OPERATIONS_ERROR);
		}

		/*
		 * Check that KEY1 is there
		 */
		ret = ltdb->kv_ops->lock_read(ldb->modules);
		if (ret != LDB_SUCCESS) {
			print_error(__location__": unlock_read returned (%d)\n",
				    ret);
			exit(ret);
		}
		key.data = (uint8_t *)talloc_strdup(tmp_ctx, KEY1);
		key.length = strlen(KEY1) + 1;

		ret = ltdb->kv_ops->fetch_and_parse(ltdb, key, parse, &val);
		if (ret != LDB_SUCCESS) {
			print_error(__location__": fetch_and_parse returned "
				    "(%d)\n",
				    ret);
			exit(ret);
		}

		if ((strlen(VAL1) + 1) != val.length) {
			print_error(__location__": KEY1 value lengths different"
				    ", expected (%d) actual(%d)\n",
				    (int)(strlen(VAL1) + 1), (int)val.length);
			exit(LDB_ERR_OPERATIONS_ERROR);
		}
		if (memcmp(VAL1, val.data, strlen(VAL1)) != 0) {
			print_error(__location__": KEY1 values different, "
				    "expected (%s) actual(%s)\n",
				    VAL1,
				    val.data);
			exit(LDB_ERR_OPERATIONS_ERROR);
		}

		/*
		 * Check that KEY2 is not there
		 */
		key.data = (uint8_t *)talloc_strdup(tmp_ctx, KEY2);
		key.length = strlen(KEY2 + 1);

		ret = ltdb->kv_ops->lock_read(ldb->modules);
		if (ret != LDB_SUCCESS) {
			print_error(__location__": lock_read returned (%d)\n",
				    ret);
			exit(ret);
		}

		ret = ltdb->kv_ops->fetch_and_parse(ltdb, key, parse, &val);
		if (ret != LDB_ERR_NO_SUCH_OBJECT) {
			print_error(__location__": fetch_and_parse returned "
				    "(%d)\n",
				    ret);
			exit(ret);
		}

		ret = ltdb->kv_ops->unlock_read(ldb->modules);
		if (ret != LDB_SUCCESS) {
			print_error(__location__": unlock_read returned (%d)\n",
				    ret);
			exit(ret);
		}

		exit(0);
	}
	close(to_child[0]);
	close(to_parent[1]);

	/*
	 * Begin a transaction and delete a record from the database
	 * but leave the transaction open
	 */
	ret = ltdb->kv_ops->begin_write(ltdb);
	assert_int_equal(ret, 0);

	key.data = (uint8_t *)talloc_strdup(tmp_ctx, KEY2);
	key.length = strlen(KEY2) + 1;

	ret = ltdb->kv_ops->delete(ltdb, key);
	assert_int_equal(ret, 0);
	/*
	 * Signal the child process
	 */
	ret = write(to_child[1], "GO", 2);
	assert_int_equal(2, ret);

	/*
	 * Wait for the child process to check the DB state while the
	 * transaction is active
	 */
	ret = read(to_parent[0], buf, 2);
	assert_int_equal(2, ret);

	/*
	 * commit the transaction
	 */
	ret = ltdb->kv_ops->finish_write(ltdb);
	assert_int_equal(0, ret);

	/*
	 * Signal the child process
	 */
	ret = write(to_child[1], "GO", 2);
	assert_int_equal(2, ret);

	w_pid = waitpid(pid, &wstatus, 0);
	assert_int_equal(pid, w_pid);

	assert_true(WIFEXITED(wstatus));

	assert_int_equal(WEXITSTATUS(wstatus), 0);


	TALLOC_FREE(tmp_ctx);
}


int main(int argc, const char **argv)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test_setup_teardown(
			test_add_get,
			setup,
			teardown),
		cmocka_unit_test_setup_teardown(
			test_delete,
			setup,
			teardown),
		cmocka_unit_test_setup_teardown(
			test_transaction_abort_write,
			setup,
			teardown),
		cmocka_unit_test_setup_teardown(
			test_transaction_abort_delete,
			setup,
			teardown),
		cmocka_unit_test_setup_teardown(
			test_write_outside_transaction,
			setup,
			teardown),
		cmocka_unit_test_setup_teardown(
			test_delete_outside_transaction,
			setup,
			teardown),
		cmocka_unit_test_setup_teardown(
			test_iterate,
			setup,
			teardown),
		cmocka_unit_test_setup_teardown(
			test_write_transaction_isolation,
			setup,
			teardown),
		cmocka_unit_test_setup_teardown(
			test_delete_transaction_isolation,
			setup,
			teardown),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
