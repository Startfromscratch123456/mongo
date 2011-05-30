/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "btree.i"

static int __wt_bulk_col(CURSOR_BULK *);
static int __wt_bulk_col_page(CURSOR_BULK *);
static int __wt_bulk_row(CURSOR_BULK *);
static int __wt_bulk_row_page(CURSOR_BULK *);

/*
 * __wt_bulk_init --
 *	Start a bulk load.
 */
int
__wt_bulk_init(CURSOR_BULK *cbulk)
{
	WT_BTREE *btree;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)cbulk->cbt.iface.session;
	btree = session->btree;

	/*
	 * You can't bulk-load into existing trees; while checking, free the
	 * empty page created when the btree was opened.
	 */
	if (F_ISSET(btree->root_page.page, WT_PAGE_INITIAL_EMPTY)) {
		btree->root_page.state = WT_REF_DISK;
		__wt_free(session, btree->root_page.page);
	} else {
		__wt_errx(
		    session, "bulk-load is only possible for empty trees");
		return (WT_ERROR);
	}

	/* Tell the eviction thread to ignore us, we'll handle our own pages. */
	F_SET(btree, WT_BTREE_NO_EVICTION);

	switch (btree->type) {
	case BTREE_COL_FIX:
		cbulk->recno = 1;
		cbulk->updp = &cbulk->upd_base;
		cbulk->page_type = WT_PAGE_COL_FIX;
		break;
	case BTREE_COL_RLE:
		cbulk->recno = 1;
		cbulk->updp = &cbulk->upd_base;
		cbulk->page_type = WT_PAGE_COL_RLE;
		break;
	case BTREE_COL_VAR:
		cbulk->recno = 1;
		cbulk->updp = &cbulk->upd_base;
		cbulk->page_type = WT_PAGE_COL_VAR;
		break;
	case BTREE_ROW:
		cbulk->insp = &cbulk->ins_base;
		cbulk->page_type = WT_PAGE_ROW_LEAF;
		break;
	}
	cbulk->ipp = 50000;			/* XXX */

	return (0);
}

/*
 * __wt_bulk_insert --
 *	Bulk insert.
 */
int
__wt_bulk_insert(CURSOR_BULK *cbulk)
{
	WT_BTREE *btree;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)cbulk->cbt.iface.session;
	btree = session->btree;

	/*
	 * The WiredTiger reconciliation code is where on-disk page formats are
	 * defined -- the goal of bulk load is to build an in-memory page that
	 * contains a set of K/V pairs which can be handed to reconciliation,
	 * which does the real work of building the on-disk pages.
	 *
	 * Basically, bulk load creates an in-memory leaf page and then loops,
	 * copying application K/V pairs into per-thread memory and pointing to
	 * the K/V pairs from the page.  When the page references enough items,
	 * the page is handed to reconciliation which builds and writes a
	 * disk-image, then discards the page.  For each of those leaf pages,
	 * bulk tracks where it ends up, and when bulk load completes, a single
	 * internal page is created which is also passed to reconciliation.
	 */
	switch (cbulk->page_type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_RLE:
	case WT_PAGE_COL_VAR:
		WT_RET(__wt_bulk_col(cbulk));
		break;
	case WT_PAGE_ROW_LEAF:
		WT_RET(__wt_bulk_row(cbulk));
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	WT_STAT_INCR(btree->stats, items_inserted);
	return (0);
}

/*
 * __wt_bulk_col --
 *	Column-store bulk load.
 */
static int
__wt_bulk_col(CURSOR_BULK *cbulk)
{
	WT_SESSION_IMPL *session;
	WT_CURSOR *cursor;
	WT_UPDATE *upd;
	int ret;

	session = (WT_SESSION_IMPL *)cbulk->cbt.iface.session;
	cursor = &cbulk->cbt.iface;
	upd = NULL;

	/*
	 * Allocate an WT_UPDATE item and append the V object onto the page's
	 * update list.
	 */
	WT_RET(__wt_update_alloc(session, (WT_ITEM *)&cursor->value, &upd));
	(*cbulk->updp) = upd;
	cbulk->updp = &upd->next;

	/* If the page is full, reconcile it and reset the insert list. */
	if (++cbulk->ins_cnt == cbulk->ipp)
		WT_ERR(__wt_bulk_col_page(cbulk));

	return (0);

err:	if (upd != NULL)
		__wt_sb_decrement(session, upd->sb);
	return (ret);
}

/*
 * __wt_bulk_row --
 *	Variable-length row-store bulk load.
 */
static int
__wt_bulk_row(CURSOR_BULK *cbulk)
{
	WT_SESSION_IMPL *session;
	WT_CURSOR *cursor;
	WT_INSERT *ins;
	WT_UPDATE *upd;
	int ret;

	session = (WT_SESSION_IMPL *)cbulk->cbt.iface.session;
	cursor = &cbulk->cbt.iface;
	ins = NULL;
	upd = NULL;

	/*
	 * Allocate a WT_INSERT/WT_UPDATE pair and append the K/V pair onto the
	 * page's insert list.
	 */
	WT_RET(__wt_row_insert_alloc(session, (WT_ITEM *)&cursor->key, &ins));
	WT_ERR(
	    __wt_update_alloc(session, (WT_ITEM *)&cursor->value, &ins->upd));
	(*cbulk->insp) = ins;
	cbulk->insp = &ins->next;

	/* If the page is full, reconcile it and reset the insert list. */
	if (++cbulk->ins_cnt == cbulk->ipp)
		WT_ERR(__wt_bulk_row_page(cbulk));

	return (0);

err:	if (ins != NULL)
		__wt_sb_decrement(session, ins->sb);
	if (upd != NULL)
		__wt_sb_decrement(session, upd->sb);
	return (ret);
}

/*
 * __wt_bulk_end --
 *	Clean up after a bulk load.
 */
int
__wt_bulk_end(CURSOR_BULK *cbulk)
{
	WT_SESSION_IMPL *session;
	WT_PAGE *page;
	WT_REF *root_page;

	session = (WT_SESSION_IMPL *)cbulk->cbt.iface.session;

	/* If the page has entries, reconcile and discard it. */
	if (cbulk->ins_cnt != 0)
		switch (cbulk->page_type) {
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_RLE:
		case WT_PAGE_COL_VAR:
			WT_RET(__wt_bulk_col_page(cbulk));
			break;
		case WT_PAGE_ROW_LEAF:
			WT_RET(__wt_bulk_row_page(cbulk));
			break;
		}

	root_page = &session->btree->root_page;

	/* Allocate an internal page and initialize it. */
	WT_RET(__wt_calloc_def(session, 1, &page));
	page->parent = NULL;				/* Root page */
	page->parent_ref = root_page;
	page->read_gen = 0;
	WT_PAGE_SET_MODIFIED(page);

	switch (cbulk->page_type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_RLE:
	case WT_PAGE_COL_VAR:
		page->entries = cbulk->ref_next;
		page->u.col_int.recno = 1;
		page->u.col_int.t = cbulk->cref;
		page->type = WT_PAGE_COL_INT;
		break;
	case WT_PAGE_ROW_LEAF:
		page->entries = cbulk->ref_next;
		page->u.row_int.t = cbulk->rref;
		page->type = WT_PAGE_ROW_INT;
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	/* Reference this page from the root of the tree. */
	root_page->state = WT_REF_MEM;
	root_page->addr = WT_ADDR_INVALID;
	root_page->size = 0;
	root_page->page = page;

	return (__wt_page_reconcile(
	    session, page, 0, WT_REC_EVICT | WT_REC_LOCKED));
}

/*
 * __wt_bulk_row_page --
 *	Reconcile a set of row-store bulk-loaded items.
 */
static int
__wt_bulk_row_page(CURSOR_BULK *cbulk)
{
	WT_SESSION_IMPL *session;
	WT_PAGE *page;
	WT_ROW_REF *rref;

	session = (WT_SESSION_IMPL *)cbulk->cbt.iface.session;

	/*
	 * Take a copy of the first key for the parent; re-allocate the parent
	 * reference array as necessary.
	 */
	if (cbulk->ref_next == cbulk->ref_entries) {
		WT_RET(__wt_realloc(session, &cbulk->ref_allocated,
		    (cbulk->ref_entries + 1000) * sizeof(*cbulk->rref),
		    &cbulk->rref));
		cbulk->ref_entries += 1000;
	}
	WT_RET(__wt_buf_set(session, &cbulk->key,
	    WT_INSERT_KEY(cbulk->ins_base),
	    WT_INSERT_KEY_SIZE(cbulk->ins_base)));
	rref = &cbulk->rref[cbulk->ref_next];
	__wt_buf_steal(session, &cbulk->key, &rref->key, &rref->size);

	/*
	 * Allocate a page.  Bulk load pages are skeleton pages: there's no
	 * underlying WT_PAGE_DISK image and each K/V pair is represented by
	 * a WT_INSERT/WT_UPDATE pair, held in a single, forward-linked list.
	 */
	WT_RET(__wt_calloc_def(session, 1, &page));
	page->parent = NULL;
	page->parent_ref = &cbulk->rref[cbulk->ref_next].ref;
	page->read_gen = __wt_cache_read_gen(session);
	page->u.bulk.recno = 0;
	page->u.bulk.ins = cbulk->ins_base;
	page->dsk = NULL;
	page->type = WT_PAGE_ROW_LEAF;
	WT_PAGE_SET_MODIFIED(page);
	F_SET(page, WT_PAGE_BULK_LOAD);

	cbulk->insp = &cbulk->ins_base;	/* The page owns the insert list */
	cbulk->ins_cnt = 0;

	++cbulk->ref_next;		/* Move to the next parent slot */

	return (__wt_page_reconcile(
	    session, page, 0, WT_REC_EVICT | WT_REC_LOCKED));
}

/*
 * __wt_bulk_col_page --
 *	Reconcile a set of column-store bulk-loaded items.
 */
static int
__wt_bulk_col_page(CURSOR_BULK *cbulk)
{
	WT_SESSION_IMPL *session;
	WT_PAGE *page;

	session = (WT_SESSION_IMPL *)cbulk->cbt.iface.session;

	/*
	 * Take a copy of the first key for the parent; re-allocate the parent
	 * reference array as necessary.
	 */
	if (cbulk->ref_next == cbulk->ref_entries) {
		WT_RET(__wt_realloc(session, &cbulk->ref_allocated,
		    (cbulk->ref_entries + 1000) * sizeof(*cbulk->cref),
		    &cbulk->cref));
		cbulk->ref_entries += 1000;
	}
	cbulk->cref[cbulk->ref_next].recno = cbulk->recno;

	/*
	 * Allocate a page.  Bulk load pages are skeleton pages: there's no
	 * underlying WT_PAGE_DISK image and each V object is represented by
	 * a WT_UPDATE item, held in a single, forward-linked list.
	 */
	WT_RET(__wt_calloc_def(session, 1, &page));
	page->parent = NULL;
	page->parent_ref = &cbulk->cref[cbulk->ref_next].ref;
	page->read_gen = __wt_cache_read_gen(session);
	page->u.bulk.recno = cbulk->recno;
	page->u.bulk.upd = cbulk->upd_base;
	page->dsk = NULL;
	page->type = cbulk->page_type;
	WT_PAGE_SET_MODIFIED(page);
	F_SET(page, WT_PAGE_BULK_LOAD);

	cbulk->recno += cbulk->ins_cnt;	/* Update the starting record number */

	cbulk->updp = &cbulk->upd_base;	/* The page owns the update list */
	cbulk->ins_cnt = 0;

	++cbulk->ref_next;		/* Move to the next parent slot */

	return (__wt_page_reconcile(
	    session, page, 0, WT_REC_EVICT | WT_REC_LOCKED));
}
