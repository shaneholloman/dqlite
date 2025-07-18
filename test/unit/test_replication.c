#include "../lib/cluster.h"
#include "../lib/runner.h"

#include "../../src/format.h"
#include "../../src/leader.h"

TEST_MODULE(replication_v1);

/******************************************************************************
 *
 * Fixture
 *
 ******************************************************************************/

#define FIXTURE                           \
	FIXTURE_CLUSTER;                  \
	struct leader leaders[N_SERVERS]; \
	sqlite3_stmt *stmt;

#define SETUP                                               \
	unsigned i;                                         \
	pool_ut_fallback()->flags |= POOL_FOR_UT_NOT_ASYNC; \
	pool_ut_fallback()->flags |= POOL_FOR_UT;           \
	SETUP_CLUSTER(V2)                                   \
	for (i = 0; i < N_SERVERS; i++) {                   \
		SETUP_LEADER(i);                            \
	}

#define SETUP_LEADER(I)                                           \
	do {                                                      \
		struct leader *leader = &f->leaders[I];           \
		struct registry *registry = CLUSTER_REGISTRY(I);  \
		struct db *db;                                    \
		int rc2;                                          \
		rc2 = registry__db_get(registry, "test.db", &db); \
		munit_assert_int(rc2, ==, 0);                     \
		rc2 = leader__init(leader, db, CLUSTER_RAFT(I));  \
		munit_assert_int(rc2, ==, 0);                     \
	} while (0)

#define TEAR_DOWN                         \
	unsigned i;                       \
	for (i = 0; i < N_SERVERS; i++) { \
		TEAR_DOWN_LEADER(i);      \
	}                                 \
	TEAR_DOWN_CLUSTER

#define TEAR_DOWN_LEADER(I)                             \
	do {                                            \
		struct leader *leader = &f->leaders[I]; \
		leader__close(leader, fixture_leader_close_cb);                  \
	} while (0)

/******************************************************************************
 *
 * Helper macros.
 *
 ******************************************************************************/

/* Return the i'th leader object. */
#define LEADER(I) &f->leaders[I]

/* Return the SQLite connection of the i'th leader object */
#define CONN(I) (LEADER(I))->conn

/* Prepare the fixture's statement using the connection of the I'th leader */
#define PREPARE(I, SQL)                                                     \
	{                                                                   \
		int rc2;                                                    \
		rc2 = sqlite3_prepare_v2(CONN(I), SQL, -1, &f->stmt, NULL); \
		munit_assert_int(rc2, ==, 0);                               \
	}

/* Reset the fixture's statement, expecting the given return code. */
#define RESET(RC)                              \
	{                                      \
		int rc2;                       \
		rc2 = sqlite3_reset(f->stmt);  \
		munit_assert_int(rc2, ==, RC); \
	}

/* Finalize the fixture's statement */
#define FINALIZE                                 \
	{                                        \
		int rc2;                         \
		rc2 = sqlite3_finalize(f->stmt); \
		munit_assert_int(rc2, ==, 0);    \
	}

/* Submit an exec request using the I'th leader. */
#define EXEC(I)                                                        \
	{                                                                  \
		f->invoked = false;                                            \
		f->req.stmt = f->stmt;                                         \
		leader_exec(LEADER(I), &f->req,                          \
				    fixture_exec_work_cb, fixture_exec_done_cb);       \
		raft_fixture_step_until(&f->cluster, fixture_invoked, f, 100); \
		munit_assert_true(f->invoked);                                 \
	}

/* Convenience to prepare, execute and finalize a statement. */
#define EXEC_SQL(I, SQL)                        \
	PREPARE(I, SQL);                        \
	EXEC(I);                                \
	CLUSTER_APPLIED(CLUSTER_LAST_INDEX(I)); \
	FINALIZE

/******************************************************************************
 *
 * Helper macros.
 *
 ******************************************************************************/

/* Assert the number of pages in the WAL file on the I'th node. */
#define ASSERT_WAL_PAGES(I, N)                                           \
	{                                                                \
		struct leader *leader_ = &f->leaders[I];                 \
		sqlite3_file *file_;                                     \
		sqlite_int64 size_;                                      \
		int pages_;                                              \
		int rv_;                                                 \
		rv_ = sqlite3_file_control(leader_->conn, NULL,          \
					   SQLITE_FCNTL_JOURNAL_POINTER, \
					   &file_);                      \
		munit_assert_int(rv_, ==, 0);                            \
		rv_ = file_->pMethods->xFileSize(file_, &size_);         \
		munit_assert_int(rv_, ==, 0);                            \
		pages_ = formatWalCalcFramesNumber(                      \
		    leader_->db->config->page_size, size_);              \
		munit_assert_int(pages_, ==, N);                         \
	}

/******************************************************************************
 *
 * leader__init
 *
 ******************************************************************************/

struct init_fixture {
	FIXTURE;
};

static void fixture_leader_close_cb(struct leader *leader) { (void)leader; }

TEST_SUITE(init);
TEST_SETUP(init)
{
	struct init_fixture *f = munit_malloc(sizeof *f);
	SETUP;
	return f;
}
TEST_TEAR_DOWN(init)
{
	struct init_fixture *f = data;
	TEAR_DOWN;
	free(f);
}

/* The connection is open and can be used. */
TEST_CASE(init, conn, NULL)
{
	struct init_fixture *f = data;
	sqlite3_stmt *stmt;
	int rc;
	(void)params;
	rc = sqlite3_prepare_v2(CONN(0), "SELECT 1", -1, &stmt, NULL);
	munit_assert_int(rc, ==, 0);
	sqlite3_finalize(stmt);
	return MUNIT_OK;
}

/******************************************************************************
 *
 * leader_exec
 *
 ******************************************************************************/

struct exec_fixture {
	FIXTURE;
	struct exec req;
	bool invoked;
	int status;
};


static void fixture_exec_work_cb(struct exec *req)
{
	int rv = sqlite3_step(req->stmt);
	sqlite3_reset(req->stmt);

	if (rv == SQLITE_DONE) {
		leader_exec_result(req, RAFT_OK);
	} else {
		leader_exec_result(req, RAFT_ERROR);
	}
	return leader_exec_resume(req);
}

static void fixture_exec_done_cb(struct exec *req)
{
	struct exec_fixture *f = req->data;
	f->invoked = true;
	f->status = req->status;
}

static bool fixture_invoked(struct raft_fixture *fixture, void *data)
{
	(void)fixture;
	struct exec_fixture *f = data;
	return f->invoked;
}

TEST_SUITE(exec);
TEST_SETUP(exec)
{
	struct exec_fixture *f = munit_malloc(sizeof *f);
	SETUP;
	f->req.data = f;
	f->req.timer.data = &f->req;
	return f;
}
TEST_TEAR_DOWN(exec)
{
	struct exec_fixture *f = data;
	TEAR_DOWN;
	free(f);
}

TEST_CASE(exec, success, NULL)
{
	struct exec_fixture *f = data;
	(void)params;
	CLUSTER_ELECT(0);
	PREPARE(0, "CREATE TABLE test (a  INT)");
	EXEC(0);
	CLUSTER_APPLIED(3);
	munit_assert_true(f->invoked);
	munit_assert_int(f->status, ==, 0);
	FINALIZE;
	return MUNIT_OK;
}

TEST_CASE(exec, barrier_fails, NULL)
{
	struct exec_fixture *f = data;
	(void)params;
	CLUSTER_ELECT(0);
	PREPARE(0, "CREATE TABLE test (a  INT)");
	raft_fixture_append_fault(&f->cluster, 0, 0);
	EXEC(0);
	munit_assert_true(f->invoked);
	munit_assert_int(f->status, ==, RAFT_IOERR);
	FINALIZE;
	return MUNIT_OK;
}

TEST_CASE(exec, append_fails, NULL)
{
	struct exec_fixture *f = data;
	(void)params;
	CLUSTER_ELECT(0);
	PREPARE(0, "CREATE TABLE test (a  INT)");
	raft_fixture_append_fault(&f->cluster, 0, 0);
	EXEC(0);
	munit_assert_true(f->invoked);
	munit_assert_int(f->status, ==, RAFT_IOERR);
	FINALIZE;
	return MUNIT_OK;
}

/* A snapshot is taken after applying an entry. */
TEST_CASE(exec, snapshot, NULL)
{
	struct exec_fixture *f = data;
	(void)params;
	CLUSTER_SNAPSHOT_THRESHOLD(0, 4);
	CLUSTER_ELECT(0);
	PREPARE(0, "CREATE TABLE test (n  INT)");
	EXEC(0);
	CLUSTER_APPLIED(3);
	FINALIZE;
	PREPARE(0, "INSERT INTO test(n) VALUES(1)");
	EXEC(0);
	CLUSTER_APPLIED(4);
	munit_assert_true(f->invoked);
	munit_assert_int(f->status, ==, 0);
	FINALIZE;
	return MUNIT_OK;
}

/* If a transaction is in progress, no snapshot is taken. */
TEST_CASE(exec, snapshot_busy, NULL)
{
	struct exec_fixture *f = data;
	(void)params;
	unsigned i;
	CLUSTER_SNAPSHOT_THRESHOLD(0, 4);
	CLUSTER_ELECT(0);
	EXEC_SQL(0, "PRAGMA cache_size = 1");
	EXEC_SQL(0, "CREATE TABLE test (n  INT)");
	EXEC_SQL(0, "BEGIN");
	/* Accumulate enough dirty data to fill the page cache */
	for (i = 0; i < 163; i++) {
		EXEC_SQL(0, "INSERT INTO test(n) VALUES(1)");
	}
	return MUNIT_OK;
}

/* If the WAL size grows beyond the configured threshold, checkpoint it. */
TEST_CASE(exec, checkpoint, NULL)
{
	struct exec_fixture *f = data;
	struct config *config = CLUSTER_CONFIG(0);
	(void)params;
	config->checkpoint_threshold = 3;
	CLUSTER_ELECT(0);
	EXEC_SQL(0, "CREATE TABLE test (n  INT)");
	EXEC_SQL(0, "INSERT INTO test(n) VALUES(1)");
	/* The WAL was truncated. */
	ASSERT_WAL_PAGES(0, 0);
	return MUNIT_OK;
}

/* If a read transaction is in progress, no checkpoint is taken. */
TEST_CASE(exec, checkpoint_read_lock, NULL)
{
	struct exec_fixture *f = data;
	struct config *config = CLUSTER_CONFIG(0);
	struct registry *registry = CLUSTER_REGISTRY(0);
	struct db *db;
	struct leader leader2;
	char *errmsg;
	int rv;
	(void)params;
	config->checkpoint_threshold = 3;

	CLUSTER_ELECT(0);
	EXEC_SQL(0, "CREATE TABLE test (n  INT)");

	/* Initialize another leader. */
	rv = registry__db_get(registry, "test.db", &db);
	munit_assert_int(rv, ==, 0);
	leader__init(&leader2, db, CLUSTER_RAFT(0));

	/* Start a read transaction in the other leader. */
	rv = sqlite3_exec(leader2.conn, "BEGIN", NULL, NULL, &errmsg);
	munit_assert_int(rv, ==, 0);

	rv = sqlite3_exec(leader2.conn, "SELECT * FROM test", NULL, NULL,
			  &errmsg);
	munit_assert_int(rv, ==, 0);

	EXEC_SQL(0, "INSERT INTO test(n) VALUES(1)");

	/* The WAL was not truncated. */
	ASSERT_WAL_PAGES(0, 3);

	leader__close(&leader2, fixture_leader_close_cb);

	return MUNIT_OK;
}

/******************************************************************************
 *
 * Fixture
 *
 ******************************************************************************/

struct fixture {
	FIXTURE_CLUSTER;
	struct leader leaders[N_SERVERS];
	sqlite3_stmt *stmt;
	struct exec req;
	bool invoked;
	int status;
};

static void *setUp(const MunitParameter params[], void *user_data)
{
	struct fixture *f = munit_malloc(sizeof *f);
	pool_ut_fallback()->flags |= POOL_FOR_UT_NOT_ASYNC;
	pool_ut_fallback()->flags |= POOL_FOR_UT;
	SETUP_CLUSTER(V2);
	SETUP_LEADER(0);
	f->req.data = f;
	f->req.timer.data = &f->req;
	return f;
}

static void tearDown(void *data)
{
	struct fixture *f = data;
	TEAR_DOWN_LEADER(0);
	TEAR_DOWN_CLUSTER;
	free(f);
}

SUITE(replication)

static void execCb(struct exec *req)
{
	struct fixture *f = req->data;
	f->invoked = true;
	f->status = req->status;
}

static void fixture_exec(struct fixture *f, unsigned i)
{
	f->req.stmt = f->stmt;
	leader_exec(LEADER(i), &f->req, fixture_exec_work_cb, execCb);
}

TEST(replication, exec, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;

	CLUSTER_ELECT(0);

	PREPARE(0, "BEGIN");
	fixture_exec(f, 0);
	CLUSTER_APPLIED(2);
	munit_assert_true(f->invoked);
	munit_assert_int(f->status, ==, RAFT_OK);
	f->invoked = false;
	f->status = -1;
	FINALIZE;

	PREPARE(0, "CREATE TABLE test (a  INT)");
	fixture_exec(f, 0);
	CLUSTER_STEP;
	munit_assert_true(f->invoked);
	munit_assert_int(f->status, ==, RAFT_OK);
	f->invoked = false;
	FINALIZE;

	PREPARE(0, "COMMIT");
	fixture_exec(f, 0);
	munit_assert_false(f->invoked);

	CLUSTER_APPLIED(3);
	FINALIZE;

	munit_assert_true(f->invoked);
	munit_assert_int(f->status, ==, RAFT_OK);

	PREPARE(0, "SELECT * FROM test");
	FINALIZE;

	SETUP_LEADER(1);
	PREPARE(1, "SELECT * FROM test");
	FINALIZE;
	TEAR_DOWN_LEADER(1);

	return MUNIT_OK;
}

/* If the WAL size grows beyond the configured threshold, checkpoint it. */
TEST(replication, checkpoint, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	struct config *config = CLUSTER_CONFIG(0);

	config->checkpoint_threshold = 3;

	CLUSTER_ELECT(0);

	PREPARE(0, "CREATE TABLE test (n  INT)");
	fixture_exec(f, 0);
	CLUSTER_APPLIED(3);
	FINALIZE;

	PREPARE(0, "INSERT INTO test(n) VALUES(1)");
	fixture_exec(f, 0);
	CLUSTER_APPLIED(5);
	FINALIZE;

	/* The WAL was truncated. */
	ASSERT_WAL_PAGES(0, 0);

	return MUNIT_OK;
}
