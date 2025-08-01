/// @file diff.c
///
/// Code for diff'ing two, three or four buffers.
///
/// There are three ways to diff:
/// - Shell out to an external diff program, using files.
/// - Use the compiled-in xdiff library.
/// - Let 'diffexpr' do the work, using files.

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auto/config.h"
#include "nvim/ascii_defs.h"
#include "nvim/autocmd.h"
#include "nvim/autocmd_defs.h"
#include "nvim/buffer.h"
#include "nvim/bufwrite.h"
#include "nvim/change.h"
#include "nvim/charset.h"
#include "nvim/cursor.h"
#include "nvim/decoration.h"
#include "nvim/diff.h"
#include "nvim/drawscreen.h"
#include "nvim/errors.h"
#include "nvim/eval.h"
#include "nvim/eval/typval.h"
#include "nvim/ex_cmds.h"
#include "nvim/ex_cmds_defs.h"
#include "nvim/ex_docmd.h"
#include "nvim/extmark.h"
#include "nvim/extmark_defs.h"
#include "nvim/fileio.h"
#include "nvim/fold.h"
#include "nvim/garray.h"
#include "nvim/garray_defs.h"
#include "nvim/gettext_defs.h"
#include "nvim/globals.h"
#include "nvim/linematch.h"
#include "nvim/mark.h"
#include "nvim/mbyte.h"
#include "nvim/mbyte_defs.h"
#include "nvim/memline.h"
#include "nvim/memline_defs.h"
#include "nvim/memory.h"
#include "nvim/message.h"
#include "nvim/move.h"
#include "nvim/normal.h"
#include "nvim/option.h"
#include "nvim/option_defs.h"
#include "nvim/option_vars.h"
#include "nvim/optionstr.h"
#include "nvim/os/fs.h"
#include "nvim/os/fs_defs.h"
#include "nvim/os/os.h"
#include "nvim/os/os_defs.h"
#include "nvim/os/shell.h"
#include "nvim/path.h"
#include "nvim/pos_defs.h"
#include "nvim/strings.h"
#include "nvim/types_defs.h"
#include "nvim/ui.h"
#include "nvim/undo.h"
#include "nvim/vim_defs.h"
#include "nvim/window.h"
#include "xdiff/xdiff.h"

static bool diff_busy = false;         // using diff structs, don't change them
static bool diff_need_update = false;  // ex_diffupdate needs to be called

// Flags obtained from the 'diffopt' option
#define DIFF_FILLER     0x001   // display filler lines
#define DIFF_IBLANK     0x002   // ignore empty lines
#define DIFF_ICASE      0x004   // ignore case
#define DIFF_IWHITE     0x008   // ignore change in white space
#define DIFF_IWHITEALL  0x010   // ignore all white space changes
#define DIFF_IWHITEEOL  0x020   // ignore change in white space at EOL
#define DIFF_HORIZONTAL 0x040   // horizontal splits
#define DIFF_VERTICAL   0x080   // vertical splits
#define DIFF_HIDDEN_OFF 0x100   // diffoff when hidden
#define DIFF_INTERNAL   0x200   // use internal xdiff algorithm
#define DIFF_CLOSE_OFF  0x400   // diffoff when closing window
#define DIFF_FOLLOWWRAP 0x800   // follow the wrap option
#define DIFF_LINEMATCH  0x1000  // match most similar lines within diff
#define DIFF_INLINE_NONE    0x2000  // no inline highlight
#define DIFF_INLINE_SIMPLE  0x4000  // inline highlight with simple algorithm
#define DIFF_INLINE_CHAR    0x8000  // inline highlight with character diff
#define DIFF_INLINE_WORD    0x10000  // inline highlight with word diff
#define DIFF_ANCHOR     0x20000  // use 'diffanchors' to anchor the diff
#define ALL_WHITE_DIFF (DIFF_IWHITE | DIFF_IWHITEALL | DIFF_IWHITEEOL)
#define ALL_INLINE (DIFF_INLINE_NONE | DIFF_INLINE_SIMPLE | DIFF_INLINE_CHAR | DIFF_INLINE_WORD)
#define ALL_INLINE_DIFF (DIFF_INLINE_CHAR | DIFF_INLINE_WORD)
static int diff_flags = DIFF_INTERNAL | DIFF_FILLER | DIFF_CLOSE_OFF;

static int diff_algorithm = 0;
static int linematch_lines = 0;

#define LBUFLEN 50               // length of line in diff file

// kTrue when "diff -a" works, kFalse when it doesn't work,
// kNone when not checked yet
static TriState diff_a_works = kNone;

enum { MAX_DIFF_ANCHORS = 20, };

// used for diff input
typedef struct {
  char *din_fname;   // used for external diff
  mmfile_t din_mmfile;  // used for internal diff
} diffin_T;

// used for diff result
typedef struct {
  char *dout_fname;  // used for external diff
  garray_T dout_ga;     // used for internal diff
} diffout_T;

// used for recording hunks from xdiff
typedef struct {
  linenr_T lnum_orig;
  int count_orig;
  linenr_T lnum_new;
  int count_new;
} diffhunk_T;

// two diff inputs and one result
typedef struct {
  diffin_T dio_orig;      // original file input
  diffin_T dio_new;       // new file input
  diffout_T dio_diff;      // diff result
  int dio_internal;  // using internal diff
} diffio_T;

typedef enum {
  DIFF_ED,
  DIFF_UNIFIED,
  DIFF_NONE,
} diffstyle_T;

#ifdef INCLUDE_GENERATED_DECLARATIONS
# include "diff.c.generated.h"
#endif

#define FOR_ALL_DIFFBLOCKS_IN_TAB(tp, dp) \
  for ((dp) = (tp)->tp_first_diff; (dp) != NULL; (dp) = (dp)->df_next)

static void clear_diffblock(diff_T *dp)
{
  ga_clear(&dp->df_changes);
  xfree(dp);
}

/// Called when deleting or unloading a buffer: No longer make a diff with it.
///
/// @param buf
void diff_buf_delete(buf_T *buf)
{
  FOR_ALL_TABS(tp) {
    int i = diff_buf_idx(buf, tp);

    if (i != DB_COUNT) {
      tp->tp_diffbuf[i] = NULL;
      tp->tp_diff_invalid = true;

      if (tp == curtab) {
        // don't redraw right away, more might change or buffer state
        // is invalid right now
        need_diff_redraw = true;
        redraw_later(curwin, UPD_VALID);
      }
    }
  }
}

/// Check if the current buffer should be added to or removed from the list of
/// diff buffers.
///
/// @param win
void diff_buf_adjust(win_T *win)
{
  if (!win->w_p_diff) {
    // When there is no window showing a diff for this buffer, remove
    // it from the diffs.
    bool found_win = false;
    FOR_ALL_WINDOWS_IN_TAB(wp, curtab) {
      if ((wp->w_buffer == win->w_buffer) && wp->w_p_diff) {
        found_win = true;
      }
    }

    if (!found_win) {
      int i = diff_buf_idx(win->w_buffer, curtab);
      if (i != DB_COUNT) {
        curtab->tp_diffbuf[i] = NULL;
        curtab->tp_diff_invalid = true;
        diff_redraw(true);
      }
    }
  } else {
    diff_buf_add(win->w_buffer);
  }
}

/// Add a buffer to make diffs for.
///
/// Call this when a new buffer is being edited in the current window where
/// 'diff' is set.
/// Marks the current buffer as being part of the diff and requiring updating.
/// This must be done before any autocmd, because a command may use info
/// about the screen contents.
///
/// @param buf The buffer to add.
void diff_buf_add(buf_T *buf)
{
  if (diff_buf_idx(buf, curtab) != DB_COUNT) {
    // It's already there.
    return;
  }

  for (int i = 0; i < DB_COUNT; i++) {
    if (curtab->tp_diffbuf[i] == NULL) {
      curtab->tp_diffbuf[i] = buf;
      curtab->tp_diff_invalid = true;
      diff_redraw(true);
      return;
    }
  }

  semsg(_("E96: Cannot diff more than %" PRId64 " buffers"), (int64_t)DB_COUNT);
}

/// Remove all buffers to make diffs for.
static void diff_buf_clear(void)
{
  for (int i = 0; i < DB_COUNT; i++) {
    if (curtab->tp_diffbuf[i] != NULL) {
      curtab->tp_diffbuf[i] = NULL;
      curtab->tp_diff_invalid = true;
      diff_redraw(true);
    }
  }
}

/// Find buffer "buf" in the list of diff buffers for tab page "tp".
///
/// @param buf
/// @param tp
///
/// @return its index or DB_COUNT if not found.
static int diff_buf_idx(buf_T *buf, tabpage_T *tp)
{
  int idx;
  for (idx = 0; idx < DB_COUNT; idx++) {
    if (tp->tp_diffbuf[idx] == buf) {
      break;
    }
  }
  return idx;
}

/// Mark the diff info involving buffer "buf" as invalid, it will be updated
/// when info is requested.
///
/// @param buf
void diff_invalidate(buf_T *buf)
{
  FOR_ALL_TABS(tp) {
    int i = diff_buf_idx(buf, tp);
    if (i != DB_COUNT) {
      tp->tp_diff_invalid = true;
      if (tp == curtab) {
        diff_redraw(true);
      }
    }
  }
}

/// Called by mark_adjust(): update line numbers in "buf".
///
/// @param line1
/// @param line2
/// @param amount
/// @param amount_after
void diff_mark_adjust(buf_T *buf, linenr_T line1, linenr_T line2, linenr_T amount,
                      linenr_T amount_after)
{
  // Handle all tab pages that use "buf" in a diff.
  FOR_ALL_TABS(tp) {
    int idx = diff_buf_idx(buf, tp);
    if (idx != DB_COUNT) {
      diff_mark_adjust_tp(tp, idx, line1, line2, amount, amount_after);
    }
  }
}

/// Update line numbers in tab page "tp" for the buffer with index "idx".
///
/// This attempts to update the changes as much as possible:
/// When inserting/deleting lines outside of existing change blocks, create a
/// new change block and update the line numbers in following blocks.
/// When inserting/deleting lines in existing change blocks, update them.
///
/// @param tp
/// @param idx
/// @param line1
/// @param line2
/// @param amount
/// @amount_after
static void diff_mark_adjust_tp(tabpage_T *tp, int idx, linenr_T line1, linenr_T line2,
                                linenr_T amount, linenr_T amount_after)
{
  if (diff_internal()) {
    // Will update diffs before redrawing.  Set _invalid to update the
    // diffs themselves, set _update to also update folds properly just
    // before redrawing.
    // Do update marks here, it is needed for :%diffput.
    tp->tp_diff_invalid = true;
    tp->tp_diff_update = true;
  }

  linenr_T inserted;
  linenr_T deleted;
  if (line2 == MAXLNUM) {
    // mark_adjust(99, MAXLNUM, 9, 0): insert lines
    inserted = amount;
    deleted = 0;
  } else if (amount_after > 0) {
    // mark_adjust(99, 98, MAXLNUM, 9): a change that inserts lines
    inserted = amount_after;
    deleted = 0;
  } else {
    // mark_adjust(98, 99, MAXLNUM, -2): delete lines
    inserted = 0;
    deleted = -amount_after;
  }

  diff_T *dprev = NULL;
  diff_T *dp = tp->tp_first_diff;

  linenr_T lnum_deleted = line1;  // lnum of remaining deletion
  while (true) {
    // If the change is after the previous diff block and before the next
    // diff block, thus not touching an existing change, create a new diff
    // block.  Don't do this when ex_diffgetput() is busy.
    if (((dp == NULL)
         || (dp->df_lnum[idx] - 1 > line2)
         || ((line2 == MAXLNUM) && (dp->df_lnum[idx] > line1)))
        && ((dprev == NULL)
            || (dprev->df_lnum[idx] + dprev->df_count[idx] < line1))
        && !diff_busy) {
      diff_T *dnext = diff_alloc_new(tp, dprev, dp);

      dnext->df_lnum[idx] = line1;
      dnext->df_count[idx] = inserted;
      for (int i = 0; i < DB_COUNT; i++) {
        if ((tp->tp_diffbuf[i] != NULL) && (i != idx)) {
          if (dprev == NULL) {
            dnext->df_lnum[i] = line1;
          } else {
            dnext->df_lnum[i] = line1
                                + (dprev->df_lnum[i] + dprev->df_count[i])
                                - (dprev->df_lnum[idx] + dprev->df_count[idx]);
          }
          dnext->df_count[i] = deleted;
        }
      }
    }

    // if at end of the list, quit
    if (dp == NULL) {
      break;
    }

    // Check for these situations:
    //    1  2  3
    //    1  2  3
    // line1     2  3  4  5
    //       2  3  4  5
    //       2  3  4  5
    // line2     2  3  4  5
    //      3     5  6
    //      3     5  6

    // compute last line of this change
    linenr_T last = dp->df_lnum[idx] + dp->df_count[idx] - 1;

    // 1. change completely above line1: nothing to do
    if (last >= line1 - 1) {
      if (diff_busy) {
        // Currently in the middle of updating diff blocks. All we want
        // is to adjust the line numbers and nothing else.
        if (dp->df_lnum[idx] > line2) {
          dp->df_lnum[idx] += amount_after;
        }

        // Advance to next entry.
        dprev = dp;
        dp = dp->df_next;
        continue;
      }

      // 6. change below line2: only adjust for amount_after; also when
      // "deleted" became zero when deleted all lines between two diffs.
      if (dp->df_lnum[idx] - (deleted + inserted != 0) > line2) {
        if (amount_after == 0) {
          // nothing left to change
          break;
        }
        dp->df_lnum[idx] += amount_after;
      } else {
        bool check_unchanged = false;

        // 2. 3. 4. 5.: inserted/deleted lines touching this diff.
        if (deleted > 0) {
          linenr_T n;
          linenr_T off = 0;
          if (dp->df_lnum[idx] >= line1) {
            if (last <= line2) {
              // 4. delete all lines of diff
              if ((dp->df_next != NULL)
                  && (dp->df_next->df_lnum[idx] - 1 <= line2)) {
                // delete continues in next diff, only do
                // lines until that one
                n = dp->df_next->df_lnum[idx] - lnum_deleted;
                deleted -= n;
                n -= dp->df_count[idx];
                lnum_deleted = dp->df_next->df_lnum[idx];
              } else {
                n = deleted - dp->df_count[idx];
              }
              dp->df_count[idx] = 0;
            } else {
              // 5. delete lines at or just before top of diff
              off = dp->df_lnum[idx] - lnum_deleted;
              n = off;
              dp->df_count[idx] -= line2 - dp->df_lnum[idx] + 1;
              check_unchanged = true;
            }
            dp->df_lnum[idx] = line1;
          } else {
            if (last < line2) {
              // 2. delete at end of diff
              dp->df_count[idx] -= last - lnum_deleted + 1;

              if ((dp->df_next != NULL)
                  && (dp->df_next->df_lnum[idx] - 1 <= line2)) {
                // delete continues in next diff, only do
                // lines until that one
                n = dp->df_next->df_lnum[idx] - 1 - last;
                deleted -= dp->df_next->df_lnum[idx] - lnum_deleted;
                lnum_deleted = dp->df_next->df_lnum[idx];
              } else {
                n = line2 - last;
              }
              check_unchanged = true;
            } else {
              // 3. delete lines inside the diff
              n = 0;
              dp->df_count[idx] -= deleted;
            }
          }

          for (int i = 0; i < DB_COUNT; i++) {
            if ((tp->tp_diffbuf[i] != NULL) && (i != idx)) {
              if (dp->df_lnum[i] > off) {
                dp->df_lnum[i] -= off;
              } else {
                dp->df_lnum[i] = 1;
              }
              dp->df_count[i] += n;
            }
          }
        } else {
          if (dp->df_lnum[idx] <= line1) {
            // inserted lines somewhere in this diff
            dp->df_count[idx] += inserted;
            check_unchanged = true;
          } else {
            // inserted lines somewhere above this diff
            dp->df_lnum[idx] += inserted;
          }
        }

        if (check_unchanged) {
          // Check if inserted lines are equal, may reduce the size of the
          // diff.
          //
          // TODO(unknown): also check for equal lines in the middle and perhaps split
          // the block.
          diff_check_unchanged(tp, dp);
        }
      }
    }

    // check if this block touches the previous one, may merge them.
    if ((dprev != NULL) && !dp->is_linematched && !diff_busy
        && (dprev->df_lnum[idx] + dprev->df_count[idx] == dp->df_lnum[idx])) {
      for (int i = 0; i < DB_COUNT; i++) {
        if (tp->tp_diffbuf[i] != NULL) {
          dprev->df_count[i] += dp->df_count[i];
        }
      }
      dp = diff_free(tp, dprev, dp);
    } else {
      // Advance to next entry.
      dprev = dp;
      dp = dp->df_next;
    }
  }

  dprev = NULL;
  dp = tp->tp_first_diff;

  while (dp != NULL) {
    // All counts are zero, remove this entry.
    int i;
    for (i = 0; i < DB_COUNT; i++) {
      if ((tp->tp_diffbuf[i] != NULL) && (dp->df_count[i] != 0)) {
        break;
      }
    }

    if (i == DB_COUNT) {
      dp = diff_free(tp, dprev, dp);
    } else {
      // Advance to next entry.
      dprev = dp;
      dp = dp->df_next;
    }
  }

  if (tp == curtab) {
    // Don't redraw right away, this updates the diffs, which can be slow.
    need_diff_redraw = true;

    // Need to recompute the scroll binding, may remove or add filler
    // lines (e.g., when adding lines above w_topline). But it's slow when
    // making many changes, postpone until redrawing.
    diff_need_scrollbind = true;
  }
}

/// Allocate a new diff block and link it between "dprev" and "dp".
///
/// @param tp
/// @param dprev
/// @param dp
///
/// @return The new diff block.
static diff_T *diff_alloc_new(tabpage_T *tp, diff_T *dprev, diff_T *dp)
{
  diff_T *dnew = xcalloc(1, sizeof(*dnew));

  dnew->is_linematched = false;
  dnew->df_next = dp;
  if (dprev == NULL) {
    tp->tp_first_diff = dnew;
  } else {
    dprev->df_next = dnew;
  }

  dnew->has_changes = false;
  ga_init(&dnew->df_changes, sizeof(diffline_change_T), 20);
  return dnew;
}

static diff_T *diff_free(tabpage_T *tp, diff_T *dprev, diff_T *dp)
{
  diff_T *ret = dp->df_next;
  clear_diffblock(dp);

  if (dprev == NULL) {
    tp->tp_first_diff = ret;
  } else {
    dprev->df_next = ret;
  }

  return ret;
}

/// Check if the diff block "dp" can be made smaller for lines at the start and
/// end that are equal.  Called after inserting lines.
///
/// This may result in a change where all buffers have zero lines, the caller
/// must take care of removing it.
///
/// @param tp
/// @param dp
static void diff_check_unchanged(tabpage_T *tp, diff_T *dp)
{
  // Find the first buffers, use it as the original, compare the other
  // buffer lines against this one.
  int i_org;
  for (i_org = 0; i_org < DB_COUNT; i_org++) {
    if (tp->tp_diffbuf[i_org] != NULL) {
      break;
    }
  }

  // safety check
  if (i_org == DB_COUNT) {
    return;
  }

  if (diff_check_sanity(tp, dp) == FAIL) {
    return;
  }

  // First check lines at the top, then at the bottom.
  linenr_T off_org = 0;
  linenr_T off_new = 0;
  int dir = FORWARD;
  while (true) {
    // Repeat until a line is found which is different or the number of
    // lines has become zero.
    while (dp->df_count[i_org] > 0) {
      // Copy the line, the next ml_get() will invalidate it.
      if (dir == BACKWARD) {
        off_org = dp->df_count[i_org] - 1;
      }
      char *line_org = xstrdup(ml_get_buf(tp->tp_diffbuf[i_org], dp->df_lnum[i_org] + off_org));

      int i_new;
      for (i_new = i_org + 1; i_new < DB_COUNT; i_new++) {
        if (tp->tp_diffbuf[i_new] == NULL) {
          continue;
        }

        if (dir == BACKWARD) {
          off_new = dp->df_count[i_new] - 1;
        }

        // if other buffer doesn't have this line, it was inserted
        if ((off_new < 0) || (off_new >= dp->df_count[i_new])) {
          break;
        }

        if (diff_cmp(line_org, ml_get_buf(tp->tp_diffbuf[i_new],
                                          dp->df_lnum[i_new] + off_new)) != 0) {
          break;
        }
      }
      xfree(line_org);

      // Stop when a line isn't equal in all diff buffers.
      if (i_new != DB_COUNT) {
        break;
      }

      // Line matched in all buffers, remove it from the diff.
      for (i_new = i_org; i_new < DB_COUNT; i_new++) {
        if (tp->tp_diffbuf[i_new] != NULL) {
          if (dir == FORWARD) {
            dp->df_lnum[i_new]++;
          }
          dp->df_count[i_new]--;
        }
      }
    }

    if (dir == BACKWARD) {
      break;
    }
    dir = BACKWARD;
  }
}

/// Check if a diff block doesn't contain invalid line numbers.
/// This can happen when the diff program returns invalid results.
///
/// @param tp
/// @param dp
///
/// @return OK if the diff block doesn't contain invalid line numbers.
static int diff_check_sanity(tabpage_T *tp, diff_T *dp)
{
  for (int i = 0; i < DB_COUNT; i++) {
    if (tp->tp_diffbuf[i] != NULL) {
      if (dp->df_lnum[i] + dp->df_count[i] - 1
          > tp->tp_diffbuf[i]->b_ml.ml_line_count) {
        return FAIL;
      }
    }
  }
  return OK;
}

/// Mark all diff buffers in the current tab page for redraw.
///
/// @param dofold Also recompute the folds
void diff_redraw(bool dofold)
{
  win_T *wp_other = NULL;
  bool used_max_fill_other = false;
  bool used_max_fill_curwin = false;

  need_diff_redraw = false;
  FOR_ALL_WINDOWS_IN_TAB(wp, curtab) {
    // when closing windows or wiping buffers skip invalid window
    if (!wp->w_p_diff || !buf_valid(wp->w_buffer)) {
      continue;
    }

    redraw_later(wp, UPD_SOME_VALID);
    if (wp != curwin) {
      wp_other = wp;
    }
    if (dofold && foldmethodIsDiff(wp)) {
      foldUpdateAll(wp);
    }

    // A change may have made filler lines invalid, need to take care of
    // that for other windows.
    int n = diff_check_fill(wp, wp->w_topline);

    if (((wp != curwin) && (wp->w_topfill > 0)) || (n > 0)) {
      if (wp->w_topfill > n) {
        wp->w_topfill = MAX(n, 0);
      } else if ((n > 0) && (n > wp->w_topfill)) {
        wp->w_topfill = n;
        if (wp == curwin) {
          used_max_fill_curwin = true;
        } else if (wp_other != NULL) {
          used_max_fill_other = true;
        }
      }
      check_topfill(wp, false);
    }
  }

  if (wp_other != NULL && curwin->w_p_scb) {
    if (used_max_fill_curwin) {
      // The current window was set to use the maximum number of filler
      // lines, may need to reduce them.
      diff_set_topline(wp_other, curwin);
    } else if (used_max_fill_other) {
      // The other window was set to use the maximum number of filler
      // lines, may need to reduce them.
      diff_set_topline(curwin, wp_other);
    }
  }
}

static void clear_diffin(diffin_T *din)
{
  if (din->din_fname == NULL) {
    XFREE_CLEAR(din->din_mmfile.ptr);
  } else {
    os_remove(din->din_fname);
  }
}

static void clear_diffout(diffout_T *dout)
{
  if (dout->dout_fname == NULL) {
    ga_clear(&dout->dout_ga);
  } else {
    os_remove(dout->dout_fname);
  }
}

/// Write buffer "buf" to a memory buffer.
///
/// @param buf
/// @param din
///
/// @return FAIL for failure.
static int diff_write_buffer(buf_T *buf, mmfile_t *m, linenr_T start, linenr_T end)
{
  if (end < 0) {
    end = buf->b_ml.ml_line_count;
  }

  if (buf->b_ml.ml_flags & ML_EMPTY || end < start) {
    m->ptr = NULL;
    m->size = 0;
    return OK;
  }

  size_t len = 0;

  // xdiff requires one big block of memory with all the text.
  for (linenr_T lnum = start; lnum <= end; lnum++) {
    len += (size_t)ml_get_buf_len(buf, lnum) + 1;
  }
  char *ptr = xmalloc(len);
  m->ptr = ptr;
  m->size = (int)len;

  len = 0;
  for (linenr_T lnum = start; lnum <= end; lnum++) {
    char *s = ml_get_buf(buf, lnum);
    if (diff_flags & DIFF_ICASE) {
      while (*s != NUL) {
        int c;
        int c_len = 1;
        char cbuf[MB_MAXBYTES + 1];

        if (*s == NL) {
          c = NUL;
        } else {
          // xdiff doesn't support ignoring case, fold-case the text.
          c = utf_ptr2char(s);
          c_len = utf_char2len(c);
          c = utf_fold(c);
        }
        const int orig_len = utfc_ptr2len(s);

        if (utf_char2bytes(c, cbuf) != c_len) {
          // TODO(Bram): handle byte length difference
          // One example is Å (3 bytes) and å (2 bytes).
          memmove(ptr + len, s, (size_t)orig_len);
        } else {
          memmove(ptr + len, cbuf, (size_t)c_len);
          if (orig_len > c_len) {
            // Copy remaining composing characters
            memmove(ptr + len + c_len, s + c_len, (size_t)(orig_len - c_len));
          }
        }

        s += orig_len;
        len += (size_t)orig_len;
      }
    } else {
      size_t slen = strlen(s);
      memmove(ptr + len, s, slen);
      // NUL is represented as NL; convert
      memchrsub(ptr + len, NL, NUL, slen);
      len += slen;
    }
    ptr[len++] = NL;
  }
  return OK;
}

/// Write buffer "buf" to file or memory buffer.
///
/// Always use 'fileformat' set to "unix".
///
/// @param buf
/// @param din
///
/// @return FAIL for failure
static int diff_write(buf_T *buf, diffin_T *din, linenr_T start, linenr_T end)
{
  if (din->din_fname == NULL) {
    return diff_write_buffer(buf, &din->din_mmfile, start, end);
  }

  if (end < 0) {
    end = buf->b_ml.ml_line_count;
  }

  // Always use 'fileformat' set to "unix".
  int save_ml_flags = buf->b_ml.ml_flags;
  char *save_ff = buf->b_p_ff;
  buf->b_p_ff = xstrdup("unix");
  const bool save_cmod_flags = cmdmod.cmod_flags;
  // Writing the buffer is an implementation detail of performing the diff,
  // so it shouldn't update the '[ and '] marks.
  cmdmod.cmod_flags |= CMOD_LOCKMARKS;
  if (end < start) {
    // The line range specifies a completely empty file.
    end = start;
    buf->b_ml.ml_flags |= ML_EMPTY;
  }
  int r = buf_write(buf, din->din_fname, NULL,
                    start, end,
                    NULL, false, false, false, true);
  cmdmod.cmod_flags = save_cmod_flags;
  free_string_option(buf->b_p_ff);
  buf->b_p_ff = save_ff;
  buf->b_ml.ml_flags = save_ml_flags;
  return r;
}

static int lnum_compare(const void *s1, const void *s2)
{
  linenr_T lnum1 = *(linenr_T *)s1;
  linenr_T lnum2 = *(linenr_T *)s2;
  if (lnum1 < lnum2) {
    return -1;
  }
  if (lnum1 > lnum2) {
    return 1;
  }
  return 0;
}

/// Update the diffs for all buffers involved.
///
/// @param dio
/// @param idx_orig
/// @param eap   can be NULL
static void diff_try_update(diffio_T *dio, int idx_orig, exarg_T *eap)
{
  if (dio->dio_internal) {
    ga_init(&dio->dio_diff.dout_ga, sizeof(diffhunk_T), 100);
  } else {
    // We need three temp file names.
    dio->dio_orig.din_fname = vim_tempname();
    dio->dio_new.din_fname = vim_tempname();
    dio->dio_diff.dout_fname = vim_tempname();
    if (dio->dio_orig.din_fname == NULL
        || dio->dio_new.din_fname == NULL
        || dio->dio_diff.dout_fname == NULL) {
      goto theend;
    }
    // Check external diff is actually working.
    if (check_external_diff(dio) == FAIL) {
      goto theend;
    }
  }

  // :diffupdate!
  if (eap != NULL && eap->forceit) {
    for (int idx_new = idx_orig; idx_new < DB_COUNT; idx_new++) {
      buf_T *buf = curtab->tp_diffbuf[idx_new];
      if (buf_valid(buf)) {
        buf_check_timestamp(buf);
      }
    }
  }

  // Parse and sort diff anchors if enabled
  int num_anchors = INT_MAX;
  linenr_T anchors[DB_COUNT][MAX_DIFF_ANCHORS];
  CLEAR_FIELD(anchors);
  if (diff_flags & DIFF_ANCHOR) {
    for (int idx = 0; idx < DB_COUNT; idx++) {
      if (curtab->tp_diffbuf[idx] == NULL) {
        continue;
      }
      int buf_num_anchors = 0;
      if (parse_diffanchors(false,
                            curtab->tp_diffbuf[idx],
                            anchors[idx],
                            &buf_num_anchors) != OK) {
        emsg(_(e_failed_to_find_all_diff_anchors));
        num_anchors = 0;
        CLEAR_FIELD(anchors);
        break;
      }
      if (buf_num_anchors < num_anchors) {
        num_anchors = buf_num_anchors;
      }

      if (buf_num_anchors > 0) {
        qsort((void *)anchors[idx],
              (size_t)buf_num_anchors,
              sizeof(linenr_T),
              lnum_compare);
      }
    }
  }
  if (num_anchors == INT_MAX) {
    num_anchors = 0;
  }

  // Split the files into multiple sections by anchors. Each section starts
  // from one anchor (inclusive) and ends at the next anchor (exclusive).
  // Diff each section separately before combining the results. If we don't
  // have any anchors, we will have one big section of the entire file.
  for (int anchor_i = 0; anchor_i <= num_anchors; anchor_i++) {
    diff_T *orig_diff = NULL;
    if (anchor_i != 0) {
      orig_diff = curtab->tp_first_diff;
      curtab->tp_first_diff = NULL;
    }
    linenr_T lnum_start = (anchor_i == 0) ? 1 : anchors[idx_orig][anchor_i - 1];
    linenr_T lnum_end = (anchor_i == num_anchors) ? -1 : anchors[idx_orig][anchor_i] - 1;

    // Write the first buffer to a tempfile or mmfile_t.
    buf_T *buf = curtab->tp_diffbuf[idx_orig];
    if (diff_write(buf, &dio->dio_orig, lnum_start, lnum_end) == FAIL) {
      if (orig_diff != NULL) {
        // Clean up in-progress diff blocks
        curtab->tp_first_diff = orig_diff;
        diff_clear(curtab);
      }
      goto theend;
    }

    // Make a difference between the first buffer and every other.
    for (int idx_new = idx_orig + 1; idx_new < DB_COUNT; idx_new++) {
      buf = curtab->tp_diffbuf[idx_new];
      if (buf == NULL || buf->b_ml.ml_mfp == NULL) {
        continue;  // skip buffer that isn't loaded
      }
      lnum_start = anchor_i == 0 ? 1 : anchors[idx_new][anchor_i - 1];
      lnum_end = anchor_i == num_anchors ? -1 : anchors[idx_new][anchor_i] - 1;

      // Write the other buffer and diff with the first one.
      if (diff_write(buf, &dio->dio_new, lnum_start, lnum_end) == FAIL) {
        continue;
      }
      if (diff_file(dio) == FAIL) {
        continue;
      }

      // Read the diff output and add each entry to the diff list.
      diff_read(idx_orig, idx_new, dio);

      clear_diffin(&dio->dio_new);
      clear_diffout(&dio->dio_diff);
    }
    clear_diffin(&dio->dio_orig);

    if (anchor_i != 0) {
      // Combine the new diff blocks with the existing ones
      for (diff_T *dp = curtab->tp_first_diff; dp != NULL; dp = dp->df_next) {
        for (int idx = 0; idx < DB_COUNT; idx++) {
          if (anchors[idx][anchor_i - 1] > 0) {
            dp->df_lnum[idx] += anchors[idx][anchor_i - 1] - 1;
          }
        }
      }
      if (orig_diff != NULL) {
        diff_T *last_diff = orig_diff;
        while (last_diff->df_next != NULL) {
          last_diff = last_diff->df_next;
        }
        last_diff->df_next = curtab->tp_first_diff;
        curtab->tp_first_diff = orig_diff;
      }
    }
  }

theend:
  xfree(dio->dio_orig.din_fname);
  xfree(dio->dio_new.din_fname);
  xfree(dio->dio_diff.dout_fname);
}

/// Return true if the options are set to use the internal diff library.
/// Note that if the internal diff failed for one of the buffers, the external
/// diff will be used anyway.
int diff_internal(void)
  FUNC_ATTR_PURE
{
  return (diff_flags & DIFF_INTERNAL) != 0 && *p_dex == NUL;
}

/// Completely update the diffs for the buffers involved.
///
/// When using the external "diff" command the buffers are written to a file,
/// also for unmodified buffers (the file could have been produced by
/// autocommands, e.g. the netrw plugin).
///
/// @param eap can be NULL
void ex_diffupdate(exarg_T *eap)
{
  if (diff_busy) {
    diff_need_update = true;
    return;
  }

  int had_diffs = curtab->tp_first_diff != NULL;

  // Delete all diffblocks.
  diff_clear(curtab);
  curtab->tp_diff_invalid = false;

  // Use the first buffer as the original text.
  int idx_orig;
  for (idx_orig = 0; idx_orig < DB_COUNT; idx_orig++) {
    if (curtab->tp_diffbuf[idx_orig] != NULL) {
      break;
    }
  }

  if (idx_orig == DB_COUNT) {
    goto theend;
  }

  // Only need to do something when there is another buffer.
  int idx_new;
  for (idx_new = idx_orig + 1; idx_new < DB_COUNT; idx_new++) {
    if (curtab->tp_diffbuf[idx_new] != NULL) {
      break;
    }
  }

  if (idx_new == DB_COUNT) {
    goto theend;
  }

  // Only use the internal method if it did not fail for one of the buffers.
  diffio_T diffio = { 0 };
  diffio.dio_internal = diff_internal();

  diff_try_update(&diffio, idx_orig, eap);

  // force updating cursor position on screen
  curwin->w_valid_cursor.lnum = 0;

theend:
  // A redraw is needed if there were diffs and they were cleared, or there
  // are diffs now, which means they got updated.
  if (had_diffs || curtab->tp_first_diff != NULL) {
    diff_redraw(true);
    apply_autocmds(EVENT_DIFFUPDATED, NULL, NULL, false, curbuf);
  }
}

/// Do a quick test if "diff" really works.  Otherwise it looks like there
/// are no differences.  Can't use the return value, it's non-zero when
/// there are differences.
static int check_external_diff(diffio_T *diffio)
{
  // May try twice, first with "-a" and then without.
  bool io_error = false;
  TriState ok = kFalse;
  while (true) {
    ok = kFalse;
    FILE *fd = os_fopen(diffio->dio_orig.din_fname, "w");

    if (fd == NULL) {
      io_error = true;
    } else {
      if (fwrite("line1\n", 6, 1, fd) != 1) {
        io_error = true;
      }
      fclose(fd);
      fd = os_fopen(diffio->dio_new.din_fname, "w");

      if (fd == NULL) {
        io_error = true;
      } else {
        if (fwrite("line2\n", 6, 1, fd) != 1) {
          io_error = true;
        }
        fclose(fd);
        fd = diff_file(diffio) == OK
             ? os_fopen(diffio->dio_diff.dout_fname, "r")
             : NULL;

        if (fd == NULL) {
          io_error = true;
        } else {
          char linebuf[LBUFLEN];

          while (true) {
            // For normal diff there must be a line that contains
            // "1c1".  For unified diff "@@ -1 +1 @@".
            if (vim_fgets(linebuf, LBUFLEN, fd)) {
              break;
            }

            if (strncmp(linebuf, "1c1", 3) == 0
                || strncmp(linebuf, "@@ -1 +1 @@", 11) == 0) {
              ok = kTrue;
            }
          }
          fclose(fd);
        }
        os_remove(diffio->dio_diff.dout_fname);
        os_remove(diffio->dio_new.din_fname);
      }
      os_remove(diffio->dio_orig.din_fname);
    }

    // When using 'diffexpr' break here.
    if (*p_dex != NUL) {
      break;
    }

    // If we checked if "-a" works already, break here.
    if (diff_a_works != kNone) {
      break;
    }
    diff_a_works = ok;

    // If "-a" works break here, otherwise retry without "-a".
    if (ok) {
      break;
    }
  }

  if (!ok) {
    if (io_error) {
      emsg(_("E810: Cannot read or write temp files"));
    }
    emsg(_("E97: Cannot create diffs"));
    diff_a_works = kNone;
    return FAIL;
  }
  return OK;
}

/// Invoke the xdiff function.
static int diff_file_internal(diffio_T *diffio)
{
  xpparam_t param;
  xdemitconf_t emit_cfg;
  xdemitcb_t emit_cb;

  CLEAR_FIELD(param);
  CLEAR_FIELD(emit_cfg);
  CLEAR_FIELD(emit_cb);

  param.flags = (unsigned long)diff_algorithm;

  if (diff_flags & DIFF_IWHITE) {
    param.flags |= XDF_IGNORE_WHITESPACE_CHANGE;
  }
  if (diff_flags & DIFF_IWHITEALL) {
    param.flags |= XDF_IGNORE_WHITESPACE;
  }
  if (diff_flags & DIFF_IWHITEEOL) {
    param.flags |= XDF_IGNORE_WHITESPACE_AT_EOL;
  }
  if (diff_flags & DIFF_IBLANK) {
    param.flags |= XDF_IGNORE_BLANK_LINES;
  }

  emit_cfg.ctxlen = 0;  // don't need any diff_context here
  emit_cb.priv = &diffio->dio_diff;
  emit_cfg.hunk_func = xdiff_out;
  if (xdl_diff(&diffio->dio_orig.din_mmfile,
               &diffio->dio_new.din_mmfile,
               &param, &emit_cfg, &emit_cb) < 0) {
    emsg(_("E960: Problem creating the internal diff"));
    return FAIL;
  }
  return OK;
}

/// Make a diff between files "tmp_orig" and "tmp_new", results in "tmp_diff".
///
/// @param dio
///
/// @return OK or FAIL
static int diff_file(diffio_T *dio)
{
  char *tmp_orig = dio->dio_orig.din_fname;
  char *tmp_new = dio->dio_new.din_fname;
  char *tmp_diff = dio->dio_diff.dout_fname;
  if (*p_dex != NUL) {
    // Use 'diffexpr' to generate the diff file.
    eval_diff(tmp_orig, tmp_new, tmp_diff);
    return OK;
  }
  // Use xdiff for generating the diff.
  if (dio->dio_internal) {
    return diff_file_internal(dio);
  }

  const size_t len = (strlen(tmp_orig) + strlen(tmp_new) + strlen(tmp_diff)
                      + strlen(p_srr) + 27);
  char *const cmd = xmalloc(len);

  // We don't want $DIFF_OPTIONS to get in the way.
  if (os_env_exists("DIFF_OPTIONS", true)) {
    os_unsetenv("DIFF_OPTIONS");
  }

  // Build the diff command and execute it.  Always use -a, binary
  // differences are of no use.  Ignore errors, diff returns
  // non-zero when differences have been found.
  vim_snprintf(cmd, len, "diff %s%s%s%s%s%s%s%s %s",
               diff_a_works == kFalse ? "" : "-a ",
               "",
               (diff_flags & DIFF_IWHITE) ? "-b " : "",
               (diff_flags & DIFF_IWHITEALL) ? "-w " : "",
               (diff_flags & DIFF_IWHITEEOL) ? "-Z " : "",
               (diff_flags & DIFF_IBLANK) ? "-B " : "",
               (diff_flags & DIFF_ICASE) ? "-i " : "",
               tmp_orig, tmp_new);
  append_redir(cmd, len, p_srr, tmp_diff);
  block_autocmds();  // Avoid ShellCmdPost stuff
  call_shell(cmd,
             kShellOptFilter | kShellOptSilent | kShellOptDoOut,
             NULL);
  unblock_autocmds();
  xfree(cmd);
  return OK;
}

/// Create a new version of a file from the current buffer and a diff file.
///
/// The buffer is written to a file, also for unmodified buffers (the file
/// could have been produced by autocommands, e.g. the netrw plugin).
///
/// @param eap
void ex_diffpatch(exarg_T *eap)
{
  char *buf = NULL;
  win_T *old_curwin = curwin;
  char *newname = NULL;  // name of patched file buffer
  char *esc_name = NULL;

#ifdef UNIX
  char *fullname = NULL;
#endif

  // We need two temp file names.
  // Name of original temp file.
  char *tmp_orig = vim_tempname();
  // Name of patched temp file.
  char *tmp_new = vim_tempname();

  if ((tmp_orig == NULL) || (tmp_new == NULL)) {
    goto theend;
  }

  // Write the current buffer to "tmp_orig".
  if (buf_write(curbuf, tmp_orig, NULL,
                1, curbuf->b_ml.ml_line_count,
                NULL, false, false, false, true) == FAIL) {
    goto theend;
  }

#ifdef UNIX
  // Get the absolute path of the patchfile, changing directory below.
  fullname = FullName_save(eap->arg, false);
  esc_name = vim_strsave_shellescape(fullname != NULL ? fullname : eap->arg, true, true);
#else
  esc_name = vim_strsave_shellescape(eap->arg, true, true);
#endif
  size_t buflen = strlen(tmp_orig) + strlen(esc_name) + strlen(tmp_new) + 16;
  buf = xmalloc(buflen);

#ifdef UNIX
  char dirbuf[MAXPATHL];
  // Temporarily chdir to /tmp, to avoid patching files in the current
  // directory when the patch file contains more than one patch.  When we
  // have our own temp dir use that instead, it will be cleaned up when we
  // exit (any .rej files created).  Don't change directory if we can't
  // return to the current.
  if ((os_dirname(dirbuf, MAXPATHL) != OK)
      || (os_chdir(dirbuf) != 0)) {
    dirbuf[0] = NUL;
  } else {
    char *tempdir = vim_gettempdir();
    if (tempdir == NULL) {
      tempdir = "/tmp";
    }
    os_chdir(tempdir);
    shorten_fnames(true);
  }
#endif

  if (*p_pex != NUL) {
    // Use 'patchexpr' to generate the new file.
#ifdef UNIX
    eval_patch(tmp_orig, (fullname != NULL ? fullname : eap->arg), tmp_new);
#else
    eval_patch(tmp_orig, eap->arg, tmp_new);
#endif
  } else {
    // Build the patch command and execute it. Ignore errors.
    vim_snprintf(buf, buflen, "patch -o %s %s < %s",
                 tmp_new, tmp_orig, esc_name);
    block_autocmds();  // Avoid ShellCmdPost stuff
    call_shell(buf, kShellOptFilter, NULL);
    unblock_autocmds();
  }

#ifdef UNIX
  if (dirbuf[0] != NUL) {
    if (os_chdir(dirbuf) != 0) {
      emsg(_(e_prev_dir));
    }
    shorten_fnames(true);
  }
#endif

  // Delete any .orig or .rej file created.
  STRCPY(buf, tmp_new);
  strcat(buf, ".orig");
  os_remove(buf);
  STRCPY(buf, tmp_new);
  strcat(buf, ".rej");
  os_remove(buf);

  // Only continue if the output file was created.
  FileInfo file_info;
  bool info_ok = os_fileinfo(tmp_new, &file_info);
  uint64_t filesize = os_fileinfo_size(&file_info);
  if (!info_ok || filesize == 0) {
    emsg(_("E816: Cannot read patch output"));
  } else {
    if (curbuf->b_fname != NULL) {
      newname = xstrnsave(curbuf->b_fname, strlen(curbuf->b_fname) + 4);
      strcat(newname, ".new");
    }

    // don't use a new tab page, each tab page has its own diffs
    cmdmod.cmod_tab = 0;

    if (win_split(0, (diff_flags & DIFF_VERTICAL) ? WSP_VERT : 0) != FAIL) {
      // Pretend it was a ":split fname" command
      eap->cmdidx = CMD_split;
      eap->arg = tmp_new;
      do_exedit(eap, old_curwin);

      // check that split worked and editing tmp_new
      if ((curwin != old_curwin) && win_valid(old_curwin)) {
        // Set 'diff', 'scrollbind' on and 'wrap' off.
        diff_win_options(curwin, true);
        diff_win_options(old_curwin, true);

        if (newname != NULL) {
          // do a ":file filename.new" on the patched buffer
          eap->arg = newname;
          ex_file(eap);

          // Do filetype detection with the new name.
          if (augroup_exists("filetypedetect")) {
            do_cmdline_cmd(":doau filetypedetect BufRead");
          }
        }
      }
    }
  }

theend:
  if (tmp_orig != NULL) {
    os_remove(tmp_orig);
  }
  xfree(tmp_orig);

  if (tmp_new != NULL) {
    os_remove(tmp_new);
  }
  xfree(tmp_new);
  xfree(newname);
  xfree(buf);
#ifdef UNIX
  xfree(fullname);
#endif
  xfree(esc_name);
}

/// Split the window and edit another file, setting options to show the diffs.
///
/// @param eap
void ex_diffsplit(exarg_T *eap)
{
  win_T *old_curwin = curwin;
  bufref_T old_curbuf;
  set_bufref(&old_curbuf, curbuf);

  // Need to compute w_fraction when no redraw happened yet.
  validate_cursor(curwin);
  set_fraction(curwin);

  // don't use a new tab page, each tab page has its own diffs
  cmdmod.cmod_tab = 0;

  if (win_split(0, (diff_flags & DIFF_VERTICAL) ? WSP_VERT : 0) == FAIL) {
    return;
  }

  // Pretend it was a ":split fname" command
  eap->cmdidx = CMD_split;
  curwin->w_p_diff = true;
  do_exedit(eap, old_curwin);

  if (curwin == old_curwin) {  // split didn't work
    return;
  }

  // Set 'diff', 'scrollbind' on and 'wrap' off.
  diff_win_options(curwin, true);
  if (win_valid(old_curwin)) {
    diff_win_options(old_curwin, true);

    if (bufref_valid(&old_curbuf)) {
      // Move the cursor position to that of the old window.
      curwin->w_cursor.lnum = diff_get_corresponding_line(old_curbuf.br_buf,
                                                          old_curwin->w_cursor.lnum);
    }
  }
  // Now that lines are folded scroll to show the cursor at the same
  // relative position.
  scroll_to_fraction(curwin, curwin->w_height);
}

// Set options to show diffs for the current window.
void ex_diffthis(exarg_T *eap)
{
  // Set 'diff', 'scrollbind' on and 'wrap' off.
  diff_win_options(curwin, true);
}

static void set_diff_option(win_T *wp, bool value)
{
  win_T *old_curwin = curwin;

  curwin = wp;
  curbuf = curwin->w_buffer;
  curbuf->b_ro_locked++;
  set_option_value_give_err(kOptDiff, BOOLEAN_OPTVAL(value), OPT_LOCAL);
  curbuf->b_ro_locked--;
  curwin = old_curwin;
  curbuf = curwin->w_buffer;
}

/// Set options in window "wp" for diff mode.
///
/// @param addbuf Add buffer to diff.
void diff_win_options(win_T *wp, bool addbuf)
{
  win_T *old_curwin = curwin;

  // close the manually opened folds
  curwin = wp;
  newFoldLevel();
  curwin = old_curwin;

  // Use 'scrollbind' and 'cursorbind' when available
  if (!wp->w_p_diff) {
    wp->w_p_scb_save = wp->w_p_scb;
  }
  wp->w_p_scb = true;

  if (!wp->w_p_diff) {
    wp->w_p_crb_save = wp->w_p_crb;
  }
  wp->w_p_crb = true;
  if (!(diff_flags & DIFF_FOLLOWWRAP)) {
    if (!wp->w_p_diff) {
      wp->w_p_wrap_save = wp->w_p_wrap;
    }
    wp->w_p_wrap = false;
    wp->w_skipcol = 0;
  }

  if (!wp->w_p_diff) {
    if (wp->w_p_diff_saved) {
      free_string_option(wp->w_p_fdm_save);
    }
    wp->w_p_fdm_save = xstrdup(wp->w_p_fdm);
  }
  set_option_direct_for(kOptFoldmethod, STATIC_CSTR_AS_OPTVAL("diff"), OPT_LOCAL, 0,
                        kOptScopeWin, wp);

  if (!wp->w_p_diff) {
    wp->w_p_fen_save = wp->w_p_fen;
    wp->w_p_fdl_save = wp->w_p_fdl;

    if (wp->w_p_diff_saved) {
      free_string_option(wp->w_p_fdc_save);
    }
    wp->w_p_fdc_save = xstrdup(wp->w_p_fdc);
  }
  free_string_option(wp->w_p_fdc);
  wp->w_p_fdc = xstrdup("2");
  assert(diff_foldcolumn >= 0 && diff_foldcolumn <= 9);
  snprintf(wp->w_p_fdc, strlen(wp->w_p_fdc) + 1, "%d", diff_foldcolumn);
  wp->w_p_fen = true;
  wp->w_p_fdl = 0;
  foldUpdateAll(wp);

  // make sure topline is not halfway through a fold
  changed_window_setting(wp);
  if (vim_strchr(p_sbo, 'h') == NULL) {
    do_cmdline_cmd("set sbo+=hor");
  }

  // Save the current values, to be restored in ex_diffoff().
  wp->w_p_diff_saved = true;

  set_diff_option(wp, true);

  if (addbuf) {
    diff_buf_add(wp->w_buffer);
  }
  redraw_later(wp, UPD_NOT_VALID);
}

/// Set options not to show diffs.  For the current window or all windows.
/// Only in the current tab page.
///
/// @param eap
void ex_diffoff(exarg_T *eap)
{
  bool diffwin = false;

  FOR_ALL_WINDOWS_IN_TAB(wp, curtab) {
    if (eap->forceit ? wp->w_p_diff : (wp == curwin)) {
      // Set 'diff' off. If option values were saved in
      // diff_win_options(), restore the ones whose settings seem to have
      // been left over from diff mode.
      set_diff_option(wp, false);

      if (wp->w_p_diff_saved) {
        if (wp->w_p_scb) {
          wp->w_p_scb = wp->w_p_scb_save;
        }

        if (wp->w_p_crb) {
          wp->w_p_crb = wp->w_p_crb_save;
        }
        if (!(diff_flags & DIFF_FOLLOWWRAP)) {
          if (!wp->w_p_wrap && wp->w_p_wrap_save) {
            wp->w_p_wrap = true;
            wp->w_leftcol = 0;
          }
        }
        free_string_option(wp->w_p_fdm);
        wp->w_p_fdm = xstrdup(*wp->w_p_fdm_save ? wp->w_p_fdm_save : "manual");
        free_string_option(wp->w_p_fdc);
        wp->w_p_fdc = xstrdup(*wp->w_p_fdc_save ? wp->w_p_fdc_save : "0");

        if (wp->w_p_fdl == 0) {
          wp->w_p_fdl = wp->w_p_fdl_save;
        }
        // Only restore 'foldenable' when 'foldmethod' is not
        // "manual", otherwise we continue to show the diff folds.
        if (wp->w_p_fen) {
          wp->w_p_fen = foldmethodIsManual(wp) ? false : wp->w_p_fen_save;
        }

        foldUpdateAll(wp);
      }
      // remove filler lines
      wp->w_topfill = 0;

      // make sure topline is not halfway a fold and cursor is
      // invalidated
      changed_window_setting(wp);

      // Note: 'sbo' is not restored, it's a global option.
      diff_buf_adjust(wp);
    }
    diffwin |= wp->w_p_diff;
  }

  // Also remove hidden buffers from the list.
  if (eap->forceit) {
    diff_buf_clear();
  }

  if (!diffwin) {
    diff_need_update = false;
    curtab->tp_diff_invalid = false;
    curtab->tp_diff_update = false;
    diff_clear(curtab);
  }

  // Remove "hor" from 'scrollopt' if there are no diff windows left.
  if (!diffwin && (vim_strchr(p_sbo, 'h') != NULL)) {
    do_cmdline_cmd("set sbo-=hor");
  }
}

static bool extract_hunk_internal(diffout_T *dout, diffhunk_T *hunk, int *line_idx)
{
  bool eof = *line_idx >= dout->dout_ga.ga_len;
  if (!eof) {
    *hunk = ((diffhunk_T *)dout->dout_ga.ga_data)[(*line_idx)++];
  }
  return eof;
}

// Extract hunk by parsing the diff output from file and calculate the diffstyle.
static bool extract_hunk(FILE *fd, diffhunk_T *hunk, diffstyle_T *diffstyle)
{
  while (true) {
    char line[LBUFLEN];  // only need to hold the diff line
    if (vim_fgets(line, LBUFLEN, fd)) {
      return true;  // end of file
    }

    if (*diffstyle == DIFF_NONE) {
      // Determine diff style.
      // ed like diff looks like this:
      // {first}[,{last}]c{first}[,{last}]
      // {first}a{first}[,{last}]
      // {first}[,{last}]d{first}
      //
      // unified diff looks like this:
      // --- file1       2018-03-20 13:23:35.783153140 +0100
      // +++ file2       2018-03-20 13:23:41.183156066 +0100
      // @@ -1,3 +1,5 @@
      if (isdigit((uint8_t)(*line))) {
        *diffstyle = DIFF_ED;
      } else if ((strncmp(line, "@@ ", 3) == 0)) {
        *diffstyle = DIFF_UNIFIED;
      } else if ((strncmp(line, "--- ", 4) == 0)
                 && (vim_fgets(line, LBUFLEN, fd) == 0)
                 && (strncmp(line, "+++ ", 4) == 0)
                 && (vim_fgets(line, LBUFLEN, fd) == 0)
                 && (strncmp(line, "@@ ", 3) == 0)) {
        *diffstyle = DIFF_UNIFIED;
      } else {
        // Format not recognized yet, skip over this line.  Cygwin diff
        // may put a warning at the start of the file.
        continue;
      }
    }

    if (*diffstyle == DIFF_ED) {
      if (!isdigit((uint8_t)(*line))) {
        continue;   // not the start of a diff block
      }
      if (parse_diff_ed(line, hunk) == FAIL) {
        continue;
      }
    } else {
      assert(*diffstyle == DIFF_UNIFIED);
      if (strncmp(line, "@@ ", 3) != 0) {
        continue;   // not the start of a diff block
      }
      if (parse_diff_unified(line, hunk) == FAIL) {
        continue;
      }
    }

    // Successfully parsed diff output, can return
    return false;
  }
}

static void process_hunk(diff_T **dpp, diff_T **dprevp, int idx_orig, int idx_new, diffhunk_T *hunk,
                         bool *notsetp)
{
  diff_T *dp = *dpp;
  diff_T *dprev = *dprevp;

  // Go over blocks before the change, for which orig and new are equal.
  // Copy blocks from orig to new.
  while (dp != NULL
         && hunk->lnum_orig > dp->df_lnum[idx_orig] + dp->df_count[idx_orig]) {
    if (*notsetp) {
      diff_copy_entry(dprev, dp, idx_orig, idx_new);
    }
    dprev = dp;
    dp = dp->df_next;
    *notsetp = true;
  }

  if ((dp != NULL)
      && (hunk->lnum_orig <= dp->df_lnum[idx_orig] + dp->df_count[idx_orig])
      && (hunk->lnum_orig + hunk->count_orig >= dp->df_lnum[idx_orig])) {
    // New block overlaps with existing block(s).
    // First find last block that overlaps.
    diff_T *dpl;
    for (dpl = dp; dpl->df_next != NULL; dpl = dpl->df_next) {
      if (hunk->lnum_orig + hunk->count_orig < dpl->df_next->df_lnum[idx_orig]) {
        break;
      }
    }

    // If the newly found block starts before the old one, set the
    // start back a number of lines.
    linenr_T off = dp->df_lnum[idx_orig] - hunk->lnum_orig;

    if (off > 0) {
      for (int i = idx_orig; i < idx_new; i++) {
        if (curtab->tp_diffbuf[i] != NULL) {
          dp->df_lnum[i] -= off;
          dp->df_count[i] += off;
        }
      }
      dp->df_lnum[idx_new] = hunk->lnum_new;
      dp->df_count[idx_new] = (linenr_T)hunk->count_new;
    } else if (*notsetp) {
      // new block inside existing one, adjust new block
      dp->df_lnum[idx_new] = hunk->lnum_new + off;
      dp->df_count[idx_new] = (linenr_T)hunk->count_new - off;
    } else {
      // second overlap of new block with existing block

      // if this hunk has different orig/new counts, adjust
      // the diff block size first. When we handled the first hunk we
      // would have expanded it to fit, without knowing that this
      // hunk exists
      int orig_size_in_dp = MIN(hunk->count_orig,
                                dp->df_lnum[idx_orig] +
                                dp->df_count[idx_orig] - hunk->lnum_orig);
      int size_diff = hunk->count_new - orig_size_in_dp;
      dp->df_count[idx_new] += size_diff;

      // grow existing block to include the overlap completely
      off = hunk->lnum_new + hunk->count_new
            - (dp->df_lnum[idx_new] + dp->df_count[idx_new]);
      if (off > 0) {
        dp->df_count[idx_new] += off;
      }
    }

    // Adjust the size of the block to include all the lines to the
    // end of the existing block or the new diff, whatever ends last.
    off = (hunk->lnum_orig + (linenr_T)hunk->count_orig)
          - (dpl->df_lnum[idx_orig] + dpl->df_count[idx_orig]);

    if (off < 0) {
      // new change ends in existing block, adjust the end. We only
      // need to do this once per block or we will over-adjust.
      if (*notsetp || dp != dpl) {
        // adjusting by 'off' here is only correct if
        // there is not another hunk in this block. we
        // adjust for this when we encounter a second
        // overlap later.
        dp->df_count[idx_new] += -off;
      }
      off = 0;
    }

    for (int i = idx_orig; i < idx_new; i++) {
      if (curtab->tp_diffbuf[i] != NULL) {
        dp->df_count[i] = dpl->df_lnum[i] + dpl->df_count[i]
                          - dp->df_lnum[i] + off;
      }
    }

    // Delete the diff blocks that have been merged into one.
    diff_T *dn = dp->df_next;
    dp->df_next = dpl->df_next;

    while (dn != dp->df_next) {
      dpl = dn->df_next;
      clear_diffblock(dn);
      dn = dpl;
    }
  } else {
    // Allocate a new diffblock.
    dp = diff_alloc_new(curtab, dprev, dp);

    dp->df_lnum[idx_orig] = hunk->lnum_orig;
    dp->df_count[idx_orig] = (linenr_T)hunk->count_orig;
    dp->df_lnum[idx_new] = hunk->lnum_new;
    dp->df_count[idx_new] = (linenr_T)hunk->count_new;

    // Set values for other buffers, these must be equal to the
    // original buffer, otherwise there would have been a change
    // already.
    for (int i = idx_orig + 1; i < idx_new; i++) {
      if (curtab->tp_diffbuf[i] != NULL) {
        diff_copy_entry(dprev, dp, idx_orig, i);
      }
    }
  }
  *notsetp = false;  // "*dp" has been set
  *dpp = dp;
  *dprevp = dprev;
}

/// Read the diff output and add each entry to the diff list.
///
/// @param idx_orig idx of original file
/// @param idx_new idx of new file
/// @dout diff output
static void diff_read(int idx_orig, int idx_new, diffio_T *dio)
{
  FILE *fd = NULL;
  int line_hunk_idx = 0;  // line or hunk index
  diff_T *dprev = NULL;
  diff_T *dp = curtab->tp_first_diff;
  diffout_T *dout = &dio->dio_diff;
  bool notset = true;  // block "*dp" not set yet
  diffstyle_T diffstyle = DIFF_NONE;

  if (!dio->dio_internal) {
    fd = os_fopen(dout->dout_fname, "r");
    if (fd == NULL) {
      emsg(_("E98: Cannot read diff output"));
      return;
    }
  }

  while (true) {
    diffhunk_T hunk = { 0 };
    bool eof = dio->dio_internal
               ? extract_hunk_internal(dout, &hunk, &line_hunk_idx)
               : extract_hunk(fd, &hunk, &diffstyle);

    if (eof) {
      break;
    }

    process_hunk(&dp, &dprev, idx_orig, idx_new, &hunk, &notset);
  }

  // for remaining diff blocks orig and new are equal
  while (dp != NULL) {
    if (notset) {
      diff_copy_entry(dprev, dp, idx_orig, idx_new);
    }
    dprev = dp;
    dp = dp->df_next;
    notset = true;
  }

  if (fd != NULL) {
    fclose(fd);
  }
}

/// Copy an entry at "dp" from "idx_orig" to "idx_new".
///
/// @param dprev
/// @param dp
/// @param idx_orig
/// @param idx_new
static void diff_copy_entry(diff_T *dprev, diff_T *dp, int idx_orig, int idx_new)
{
  linenr_T off;

  if (dprev == NULL) {
    off = 0;
  } else {
    off = (dprev->df_lnum[idx_orig] + dprev->df_count[idx_orig])
          - (dprev->df_lnum[idx_new] + dprev->df_count[idx_new]);
  }
  dp->df_lnum[idx_new] = dp->df_lnum[idx_orig] - off;
  dp->df_count[idx_new] = dp->df_count[idx_orig];
}

/// Clear the list of diffblocks for tab page "tp".
///
/// @param tp
void diff_clear(tabpage_T *tp)
  FUNC_ATTR_NONNULL_ALL
{
  diff_T *next_p;
  for (diff_T *p = tp->tp_first_diff; p != NULL; p = next_p) {
    next_p = p->df_next;
    clear_diffblock(p);
  }
  tp->tp_first_diff = NULL;
}

/// Return true if the options are set to use diff linematch.
bool diff_linematch(diff_T *dp)
{
  if (!(diff_flags & DIFF_LINEMATCH)) {
    return false;
  }
  // are there more than three diff buffers?
  int tsize = 0;
  for (int i = 0; i < DB_COUNT; i++) {
    if (curtab->tp_diffbuf[i] != NULL) {
      // for the rare case (bug?) that the count of a diff block is negative, do
      // not run the algorithm because this will try to allocate a negative
      // amount of space and crash
      if (dp->df_count[i] < 0) {
        return false;
      }
      tsize += dp->df_count[i];
    }
  }
  // avoid allocating a huge array because it will lag
  return tsize <= linematch_lines;
}

static int get_max_diff_length(const diff_T *dp)
{
  int maxlength = 0;
  for (int k = 0; k < DB_COUNT; k++) {
    if (curtab->tp_diffbuf[k] != NULL) {
      if (dp->df_count[k] > maxlength) {
        maxlength = dp->df_count[k];
      }
    }
  }
  return maxlength;
}

/// Find the first diff block that includes the specified line. Also find the
/// next diff block that's not in the current chain of adjacent blocks that are
/// all touching each other directly.
static void find_top_diff_block(diff_T **thistopdiff, diff_T **next_adjacent_blocks, int fromidx,
                                int topline)
{
  diff_T *topdiff = NULL;
  diff_T *localtopdiff = NULL;
  int topdiffchange = 0;

  for (topdiff = curtab->tp_first_diff; topdiff != NULL; topdiff = topdiff->df_next) {
    // set the top of the current overlapping diff block set as we
    // iterate through all of the sets of overlapping diff blocks
    if (!localtopdiff || topdiffchange) {
      localtopdiff = topdiff;
      topdiffchange = 0;
    }

    // check if the fromwin topline is matched by the current diff. if so,
    // set it to the top of the diff block
    if (topline >= topdiff->df_lnum[fromidx] && topline <=
        (topdiff->df_lnum[fromidx] + topdiff->df_count[fromidx])) {
      // this line is inside the current diff block, so we will save the
      // top block of the set of blocks to refer to later
      if ((*thistopdiff) == NULL) {
        (*thistopdiff) = localtopdiff;
      }
    }

    // check if the next set of overlapping diff blocks is next
    if (!(topdiff->df_next && (topdiff->df_next->df_lnum[fromidx] ==
                               (topdiff->df_lnum[fromidx] + topdiff->df_count[fromidx])))) {
      // mark that the next diff block is belongs to a different set of
      // overlapping diff blocks
      topdiffchange = 1;

      // if we already have found that the line number is inside a diff block,
      // set the marker of the next block and finish the iteration
      if (*thistopdiff) {
        (*next_adjacent_blocks) = topdiff->df_next;
        break;
      }
    }
  }
}

/// Calculates topline/topfill of a target diff window to fit the source diff window.
static void calculate_topfill_and_topline(const int fromidx, const int toidx, const
                                          int from_topline, const int from_topfill, int *topfill,
                                          linenr_T *topline)
{
  // find the position from the top of the diff block, and the next diff
  // block that's no longer adjacent to the current block. "Adjacency" means
  // a chain of diff blocks that are directly touching each other, allowed by
  // linematch and diff anchors.
  diff_T *thistopdiff = NULL;
  diff_T *next_adjacent_blocks = NULL;
  int virtual_lines_passed = 0;

  find_top_diff_block(&thistopdiff, &next_adjacent_blocks, fromidx, from_topline);

  // count the virtual lines (either filler or concrete line) that have been
  // passed in the source buffer. There could be multiple diff blocks if
  // there are adjacent empty blocks (count == 0 at fromidx).

  diff_T *curdif = thistopdiff;
  while (curdif && (curdif->df_lnum[fromidx] + curdif->df_count[fromidx])
         <= from_topline) {
    virtual_lines_passed += get_max_diff_length(curdif);

    curdif = curdif->df_next;
  }

  if (curdif != next_adjacent_blocks) {
    virtual_lines_passed += from_topline - curdif->df_lnum[fromidx];
  }
  virtual_lines_passed -= from_topfill;

  // clamp negative values in case from_topfill hasn't been updated yet and
  // is larger than total virtual lines, which could happen when setting
  // diffopt multiple times
  if (virtual_lines_passed < 0) {
    virtual_lines_passed = 0;
  }

  // move the same amount of virtual lines in the target buffer to find the
  // cursor's line number
  int curlinenum_to = thistopdiff->df_lnum[toidx];

  int virt_lines_left = virtual_lines_passed;
  curdif = thistopdiff;
  while (virt_lines_left > 0 && curdif != NULL && curdif != next_adjacent_blocks) {
    curlinenum_to += MIN(virt_lines_left, curdif->df_count[toidx]);
    virt_lines_left -= MIN(virt_lines_left, get_max_diff_length(curdif));
    curdif = curdif->df_next;
  }

  // count the total number of virtual lines between the top diff block and
  // the found line in the target buffer
  int max_virt_lines = 0;
  for (diff_T *dp = thistopdiff; dp != NULL; dp = dp->df_next) {
    if (dp->df_lnum[toidx] + dp->df_count[toidx] <= curlinenum_to) {
      max_virt_lines += get_max_diff_length(dp);
    } else {
      if (dp->df_lnum[toidx] <= curlinenum_to) {
        max_virt_lines += curlinenum_to - dp->df_lnum[toidx];
      }
      break;
    }
  }

  if (diff_flags & DIFF_FILLER) {
    // should always be non-negative as max_virt_lines is larger
    (*topfill) = max_virt_lines - virtual_lines_passed;
  }
  (*topline) = curlinenum_to;
}

// Apply results from the linematch algorithm and apply to 'dp' by splitting it into multiple
// adjacent diff blocks.
static void apply_linematch_results(diff_T *dp, size_t decisions_length, const int *decisions)
{
  // get the start line number here in each diff buffer, and then increment
  int line_numbers[DB_COUNT];
  int outputmap[DB_COUNT];
  size_t ndiffs = 0;
  for (int i = 0; i < DB_COUNT; i++) {
    if (curtab->tp_diffbuf[i] != NULL) {
      line_numbers[i] = dp->df_lnum[i];
      dp->df_count[i] = 0;

      // Keep track of the index of the diff buffer we are using here.
      // We will use this to write the output of the algorithm to
      // diff_T structs at the correct indexes
      outputmap[ndiffs] = i;
      ndiffs++;
    }
  }

  // write the diffs starting with the current diff block
  diff_T *dp_s = dp;
  for (size_t i = 0; i < decisions_length; i++) {
    // Don't allocate on first iter since we can reuse the initial diffblock
    if (i != 0 && (decisions[i - 1] != decisions[i])) {
      // create new sub diff blocks to segment the original diff block which we
      // further divided by running the linematch algorithm
      dp_s = diff_alloc_new(curtab, dp_s, dp_s->df_next);
      dp_s->is_linematched = true;
      for (int j = 0; j < DB_COUNT; j++) {
        if (curtab->tp_diffbuf[j] != NULL) {
          dp_s->df_lnum[j] = line_numbers[j];
          dp_s->df_count[j] = 0;
        }
      }
    }
    for (size_t j = 0; j < ndiffs; j++) {
      if (decisions[i] & (1 << j)) {
        // will need to use the map here
        dp_s->df_count[outputmap[j]]++;
        line_numbers[outputmap[j]]++;
      }
    }
  }
  dp->is_linematched = true;
}

static void run_linematch_algorithm(diff_T *dp)
{
  // define buffers for diff algorithm
  mmfile_t diffbufs_mm[DB_COUNT];
  const mmfile_t *diffbufs[DB_COUNT];
  int diff_length[DB_COUNT];
  size_t ndiffs = 0;
  for (int i = 0; i < DB_COUNT; i++) {
    if (curtab->tp_diffbuf[i] != NULL) {
      if (dp->df_count[i] > 0) {
        // write the contents of the entire buffer to
        // diffbufs_mm[diffbuffers_count]
        diff_write_buffer(curtab->tp_diffbuf[i], &diffbufs_mm[ndiffs],
                          dp->df_lnum[i], dp->df_lnum[i] + dp->df_count[i] - 1);
      } else {
        diffbufs_mm[ndiffs].size = 0;
        diffbufs_mm[ndiffs].ptr = NULL;
      }

      diffbufs[ndiffs] = &diffbufs_mm[ndiffs];

      // keep track of the length of this diff block to pass it to the linematch
      // algorithm
      diff_length[ndiffs] = dp->df_count[i];

      // increment the amount of diff buffers we are passing to the algorithm
      ndiffs++;
    }
  }

  // we will get the output of the linematch algorithm in the format of an array
  // of integers (*decisions) and the length of that array (decisions_length)
  int *decisions = NULL;
  const bool iwhite = (diff_flags & (DIFF_IWHITEALL | DIFF_IWHITE)) > 0;
  size_t decisions_length = linematch_nbuffers(diffbufs, diff_length, ndiffs, &decisions, iwhite);

  for (size_t i = 0; i < ndiffs; i++) {
    XFREE_CLEAR(diffbufs_mm[i].ptr);
  }

  apply_linematch_results(dp, decisions_length, decisions);

  xfree(decisions);
}

/// Check diff status for line "lnum" in buffer "buf":
///
/// Returns > 0 for inserting that many filler lines above it (never happens
/// when 'diffopt' doesn't contain "filler"). Otherwise returns 0.
///
/// "linestatus" (can be NULL) will be set to:
/// 0 for nothing special.
/// -1 for a line that should be highlighted as changed.
/// -2 for a line that should be highlighted as added/deleted.
///
/// This should only be used for windows where 'diff' is set.
///
/// Note that it's possible for a changed/added/deleted line to also have filler
/// lines above it. This happens when using linematch or using diff anchors (at
/// the anchored lines).
///
/// @param wp
/// @param lnum
/// @param[out] linestatus
///
/// @return diff status.
int diff_check_with_linestatus(win_T *wp, linenr_T lnum, int *linestatus)
{
  buf_T *buf = wp->w_buffer;

  if (linestatus != NULL) {
    *linestatus = 0;
  }

  if (curtab->tp_diff_invalid) {
    // update after a big change
    ex_diffupdate(NULL);
  }

  // no diffs at all
  if ((curtab->tp_first_diff == NULL) || !wp->w_p_diff) {
    return 0;
  }

  // safety check: "lnum" must be a buffer line
  if ((lnum < 1) || (lnum > buf->b_ml.ml_line_count + 1)) {
    return 0;
  }

  int idx = diff_buf_idx(buf, curtab);  // index in tp_diffbuf[] for this buffer

  if (idx == DB_COUNT) {
    // no diffs for buffer "buf"
    return 0;
  }

  // A closed fold never has filler lines.
  if (hasFolding(wp, lnum, NULL, NULL) || decor_conceal_line(wp, lnum - 1, false)) {
    return 0;
  }

  // search for a change that includes "lnum" in the list of diffblocks.
  diff_T *dp;
  for (dp = curtab->tp_first_diff; dp != NULL; dp = dp->df_next) {
    if (lnum <= dp->df_lnum[idx] + dp->df_count[idx]) {
      break;
    }
  }

  if ((dp == NULL) || (lnum < dp->df_lnum[idx])) {
    return 0;
  }

  // Don't run linematch when lnum is offscreen.
  // Useful for scrollbind calculations which need to count all the filler lines
  // above the screen.
  if (lnum >= wp->w_topline && lnum < wp->w_botline
      && !dp->is_linematched && diff_linematch(dp)
      && diff_check_sanity(curtab, dp)) {
    run_linematch_algorithm(dp);
  }

  // Insert filler lines above the line just below the change.  Will return 0
  // when this buf had the max count.
  int num_fill = 0;
  while (lnum == dp->df_lnum[idx] + dp->df_count[idx]) {
    // Only calculate fill lines if 'diffopt' contains "filler". Otherwise
    // returns 0 filler lines.
    if (diff_flags & DIFF_FILLER) {
      int maxcount = get_max_diff_length(dp);
      num_fill += maxcount - dp->df_count[idx];
    }

    // If there are adjacent blocks (e.g. linematch or anchor), loop
    // through them. It's possible for multiple adjacent blocks to
    // contribute to filler lines.
    // This also helps us find the last diff block in the list of adjacent
    // blocks which is necessary when it is a change/inserted line right
    // after added lines.
    if (dp->df_next != NULL
        && lnum >= dp->df_next->df_lnum[idx]
        && lnum <= dp->df_next->df_lnum[idx] + dp->df_next->df_count[idx]) {
      dp = dp->df_next;
    } else {
      break;
    }
  }

  if (lnum < dp->df_lnum[idx] + dp->df_count[idx]) {
    bool zero = false;

    // Changed or inserted line.  If the other buffers have a count of
    // zero, the lines were inserted.  If the other buffers have the same
    // count, check if the lines are identical.
    bool cmp = false;

    for (int i = 0; i < DB_COUNT; i++) {
      if ((i != idx) && (curtab->tp_diffbuf[i] != NULL)) {
        if (dp->df_count[i] == 0) {
          zero = true;
        } else {
          if (dp->df_count[i] != dp->df_count[idx]) {
            if (linestatus) {
              *linestatus = -1;  // nr of lines changed.
            }
            return num_fill;
          }
          cmp = true;
        }
      }
    }

    if (cmp) {
      // Compare all lines.  If they are equal the lines were inserted
      // in some buffers, deleted in others, but not changed.
      for (int i = 0; i < DB_COUNT; i++) {
        if ((i != idx)
            && (curtab->tp_diffbuf[i] != NULL)
            && (dp->df_count[i] != 0)) {
          if (!diff_equal_entry(dp, idx, i)) {
            if (linestatus) {
              *linestatus = -1;
            }
            return num_fill;
          }
        }
      }
    }

    // If there is no buffer with zero lines then there is no difference
    // any longer.  Happens when making a change (or undo) that removes
    // the difference.  Can't remove the entry here, we might be halfway
    // through updating the window.  Just report the text as unchanged.
    // Other windows might still show the change though.
    if (!zero) {
      return num_fill;
    }
    if (linestatus) {
      *linestatus = -2;
    }
    return num_fill;
  }
  return num_fill;
}

/// See diff_check_with_linestatus
int diff_check_fill(win_T *wp, linenr_T lnum)
{
  // be quick when there are no filler lines
  if (!(diff_flags & DIFF_FILLER)) {
    return 0;
  }
  int n = diff_check_with_linestatus(wp, lnum, NULL);
  return MAX(n, 0);
}

/// Compare two entries in diff "dp" and return true if they are equal.
///
/// @param  dp    diff
/// @param  idx1  first entry in diff "dp"
/// @param  idx2  second entry in diff "dp"
///
/// @return true if two entries are equal.
static bool diff_equal_entry(diff_T *dp, int idx1, int idx2)
  FUNC_ATTR_PURE FUNC_ATTR_WARN_UNUSED_RESULT FUNC_ATTR_NONNULL_ARG(1)
{
  if (dp->df_count[idx1] != dp->df_count[idx2]) {
    return false;
  }

  if (diff_check_sanity(curtab, dp) == FAIL) {
    return false;
  }

  for (int i = 0; i < dp->df_count[idx1]; i++) {
    char *line = xstrdup(ml_get_buf(curtab->tp_diffbuf[idx1], dp->df_lnum[idx1] + i));

    int cmp = diff_cmp(line, ml_get_buf(curtab->tp_diffbuf[idx2], dp->df_lnum[idx2] + i));
    xfree(line);

    if (cmp != 0) {
      return false;
    }
  }
  return true;
}

// Compare the characters at "p1" and "p2".  If they are equal (possibly
// ignoring case) return true and set "len" to the number of bytes.
static bool diff_equal_char(const char *const p1, const char *const p2, int *const len)
{
  const int l = utfc_ptr2len(p1);

  if (l != utfc_ptr2len(p2)) {
    return false;
  }
  if (l > 1) {
    if (strncmp(p1, p2, (size_t)l) != 0
        && (!(diff_flags & DIFF_ICASE)
            || utf_fold(utf_ptr2char(p1)) != utf_fold(utf_ptr2char(p2)))) {
      return false;
    }
    *len = l;
  } else {
    if ((*p1 != *p2)
        && (!(diff_flags & DIFF_ICASE)
            || TOLOWER_LOC((uint8_t)(*p1)) != TOLOWER_LOC((uint8_t)(*p2)))) {
      return false;
    }
    *len = 1;
  }
  return true;
}

/// Compare strings "s1" and "s2" according to 'diffopt'.
/// Return non-zero when they are different.
///
/// @param s1 The first string
/// @param s2 The second string
///
/// @return on-zero if the two strings are different.
static int diff_cmp(char *s1, char *s2)
{
  if ((diff_flags & DIFF_IBLANK)
      && (*skipwhite(s1) == NUL || *skipwhite(s2) == NUL)) {
    return 0;
  }

  if ((diff_flags & (DIFF_ICASE | ALL_WHITE_DIFF)) == 0) {
    return strcmp(s1, s2);
  }

  if ((diff_flags & DIFF_ICASE) && !(diff_flags & ALL_WHITE_DIFF)) {
    return mb_stricmp(s1, s2);
  }

  char *p1 = s1;
  char *p2 = s2;

  // Ignore white space changes and possibly ignore case.
  while (*p1 != NUL && *p2 != NUL) {
    if (((diff_flags & DIFF_IWHITE)
         && ascii_iswhite(*p1) && ascii_iswhite(*p2))
        || ((diff_flags & DIFF_IWHITEALL)
            && (ascii_iswhite(*p1) || ascii_iswhite(*p2)))) {
      p1 = skipwhite(p1);
      p2 = skipwhite(p2);
    } else {
      int l;
      if (!diff_equal_char(p1, p2, &l)) {
        break;
      }
      p1 += l;
      p2 += l;
    }
  }

  // Ignore trailing white space.
  p1 = skipwhite(p1);
  p2 = skipwhite(p2);

  if ((*p1 != NUL) || (*p2 != NUL)) {
    return 1;
  }
  return 0;
}

/// Set the topline of "towin" to match the position in "fromwin", so that they
/// show the same diff'ed lines.
///
/// @param fromwin
/// @param towin
void diff_set_topline(win_T *fromwin, win_T *towin)
{
  buf_T *frombuf = fromwin->w_buffer;
  linenr_T lnum = fromwin->w_topline;

  int fromidx = diff_buf_idx(frombuf, curtab);
  if (fromidx == DB_COUNT) {
    // safety check
    return;
  }

  if (curtab->tp_diff_invalid) {
    // update after a big change
    ex_diffupdate(NULL);
  }
  towin->w_topfill = 0;

  // search for a change that includes "lnum" in the list of diffblocks.
  diff_T *dp;
  for (dp = curtab->tp_first_diff; dp != NULL; dp = dp->df_next) {
    if (lnum <= dp->df_lnum[fromidx] + dp->df_count[fromidx]) {
      break;
    }
  }

  if (dp == NULL) {
    // After last change, compute topline relative to end of file; no
    // filler lines.
    towin->w_topline = towin->w_buffer->b_ml.ml_line_count
                       - (frombuf->b_ml.ml_line_count - lnum);
  } else {
    // Find index for "towin".
    int toidx = diff_buf_idx(towin->w_buffer, curtab);

    if (toidx == DB_COUNT) {
      // safety check
      return;
    }
    towin->w_topline = lnum + (dp->df_lnum[toidx] - dp->df_lnum[fromidx]);

    if (lnum >= dp->df_lnum[fromidx]) {
      calculate_topfill_and_topline(fromidx, toidx,
                                    fromwin->w_topline, fromwin->w_topfill,
                                    &towin->w_topfill, &towin->w_topline);
    }
  }

  // safety check (if diff info gets outdated strange things may happen)
  towin->w_botfill = false;

  if (towin->w_topline > towin->w_buffer->b_ml.ml_line_count) {
    towin->w_topline = towin->w_buffer->b_ml.ml_line_count;
    towin->w_botfill = true;
  }

  if (towin->w_topline < 1) {
    towin->w_topline = 1;
    towin->w_topfill = 0;
  }

  // When w_topline changes need to recompute w_botline and cursor position
  invalidate_botline(towin);
  changed_line_abv_curs_win(towin);

  check_topfill(towin, false);
  hasFolding(towin, towin->w_topline, &towin->w_topline, NULL);
}

/// Parse the diff anchors. If "check_only" is set, will only make sure the
/// syntax is correct.
static int parse_diffanchors(bool check_only, buf_T *buf, linenr_T *anchors, int *num_anchors)
{
  int i;
  char *dia = (*buf->b_p_dia == NUL) ? p_dia : buf->b_p_dia;

  buf_T *orig_curbuf = curbuf;
  win_T *orig_curwin = curwin;

  win_T *bufwin = NULL;
  if (check_only) {
    bufwin = curwin;
  } else {
    // Find the first window tied to this buffer and ignore the rest. Will
    // only matter for window-specific addresses like `.` or `''`.
    for (bufwin = firstwin; bufwin != NULL; bufwin = bufwin->w_next) {
      if (bufwin->w_buffer == buf && bufwin->w_p_diff) {
        break;
      }
    }
    if (bufwin == NULL) {
      return FAIL;  // should not really happen
    }
  }

  for (i = 0; i < MAX_DIFF_ANCHORS && *dia != NUL; i++) {
    if (*dia == ',') {  // don't allow empty values
      return FAIL;
    }

    curbuf = buf;
    curwin = bufwin;
    const char *errormsg = NULL;
    linenr_T lnum = get_address(NULL, &dia, ADDR_LINES, check_only, true, false, 1, &errormsg);
    curbuf = orig_curbuf;
    curwin = orig_curwin;

    if (errormsg != NULL) {
      emsg(errormsg);
    }
    if (dia == NULL) {  // error detected
      return FAIL;
    }
    if (*dia != ',' && *dia != NUL) {
      return FAIL;
    }

    if (!check_only
        && (lnum == MAXLNUM || lnum <= 0 || lnum > buf->b_ml.ml_line_count + 1)) {
      emsg(_(e_invrange));
      return FAIL;
    }

    if (anchors != NULL) {
      anchors[i] = lnum;
    }

    if (*dia == ',') {
      dia++;
    }
  }
  if (i == MAX_DIFF_ANCHORS && *dia != NUL) {
    semsg(_(e_cannot_have_more_than_nr_diff_anchors), MAX_DIFF_ANCHORS);
    return FAIL;
  }
  if (num_anchors != NULL) {
    *num_anchors = i;
  }
  return OK;
}

/// This is called when 'diffanchors' is changed.
int diffanchors_changed(bool buflocal)
{
  int result = parse_diffanchors(true, curbuf, NULL, NULL);
  if (result == OK && (diff_flags & DIFF_ANCHOR)) {
    FOR_ALL_TABS(tp) {
      if (!buflocal) {
        tp->tp_diff_invalid = true;
      } else {
        for (int idx = 0; idx < DB_COUNT; idx++) {
          if (tp->tp_diffbuf[idx] == curbuf) {
            tp->tp_diff_invalid = true;
            break;
          }
        }
      }
    }
  }
  return result;
}

/// This is called when 'diffopt' is changed.
///
/// @return
int diffopt_changed(void)
{
  int diff_context_new = 6;
  int linematch_lines_new = 0;
  int diff_flags_new = 0;
  int diff_foldcolumn_new = 2;
  int diff_algorithm_new = 0;
  int diff_indent_heuristic = 0;

  char *p = p_dip;
  while (*p != NUL) {
    // Note: Keep this in sync with opt_dip_values.
    if (strncmp(p, "filler", 6) == 0) {
      p += 6;
      diff_flags_new |= DIFF_FILLER;
    } else if (strncmp(p, "anchor", 6) == 0) {
      p += 6;
      diff_flags_new |= DIFF_ANCHOR;
    } else if ((strncmp(p, "context:", 8) == 0) && ascii_isdigit(p[8])) {
      p += 8;
      diff_context_new = getdigits_int(&p, false, diff_context_new);
    } else if (strncmp(p, "iblank", 6) == 0) {
      p += 6;
      diff_flags_new |= DIFF_IBLANK;
    } else if (strncmp(p, "icase", 5) == 0) {
      p += 5;
      diff_flags_new |= DIFF_ICASE;
    } else if (strncmp(p, "iwhiteall", 9) == 0) {
      p += 9;
      diff_flags_new |= DIFF_IWHITEALL;
    } else if (strncmp(p, "iwhiteeol", 9) == 0) {
      p += 9;
      diff_flags_new |= DIFF_IWHITEEOL;
    } else if (strncmp(p, "iwhite", 6) == 0) {
      p += 6;
      diff_flags_new |= DIFF_IWHITE;
    } else if (strncmp(p, "horizontal", 10) == 0) {
      p += 10;
      diff_flags_new |= DIFF_HORIZONTAL;
    } else if (strncmp(p, "vertical", 8) == 0) {
      p += 8;
      diff_flags_new |= DIFF_VERTICAL;
    } else if ((strncmp(p, "foldcolumn:", 11) == 0) && ascii_isdigit(p[11])) {
      p += 11;
      diff_foldcolumn_new = getdigits_int(&p, false, diff_foldcolumn_new);
    } else if (strncmp(p, "hiddenoff", 9) == 0) {
      p += 9;
      diff_flags_new |= DIFF_HIDDEN_OFF;
    } else if (strncmp(p, "closeoff", 8) == 0) {
      p += 8;
      diff_flags_new |= DIFF_CLOSE_OFF;
    } else if (strncmp(p, "followwrap", 10) == 0) {
      p += 10;
      diff_flags_new |= DIFF_FOLLOWWRAP;
    } else if (strncmp(p, "indent-heuristic", 16) == 0) {
      p += 16;
      diff_indent_heuristic = XDF_INDENT_HEURISTIC;
    } else if (strncmp(p, "internal", 8) == 0) {
      p += 8;
      diff_flags_new |= DIFF_INTERNAL;
    } else if (strncmp(p, "algorithm:", 10) == 0) {
      // Note: Keep this in sync with opt_dip_algorithm_values.
      p += 10;
      if (strncmp(p, "myers", 5) == 0) {
        p += 5;
        diff_algorithm_new = 0;
      } else if (strncmp(p, "minimal", 7) == 0) {
        p += 7;
        diff_algorithm_new = XDF_NEED_MINIMAL;
      } else if (strncmp(p, "patience", 8) == 0) {
        p += 8;
        diff_algorithm_new = XDF_PATIENCE_DIFF;
      } else if (strncmp(p, "histogram", 9) == 0) {
        p += 9;
        diff_algorithm_new = XDF_HISTOGRAM_DIFF;
      } else {
        return FAIL;
      }
    } else if (strncmp(p, "inline:", 7) == 0) {
      // Note: Keep this in sync with opt_dip_inline_values.
      p += 7;
      if (strncmp(p, "none", 4) == 0) {
        p += 4;
        diff_flags_new &= ~(ALL_INLINE);
        diff_flags_new |= DIFF_INLINE_NONE;
      } else if (strncmp(p, "simple", 6) == 0) {
        p += 6;
        diff_flags_new &= ~(ALL_INLINE);
        diff_flags_new |= DIFF_INLINE_SIMPLE;
      } else if (strncmp(p, "char", 4) == 0) {
        p += 4;
        diff_flags_new &= ~(ALL_INLINE);
        diff_flags_new |= DIFF_INLINE_CHAR;
      } else if (strncmp(p, "word", 4) == 0) {
        p += 4;
        diff_flags_new &= ~(ALL_INLINE);
        diff_flags_new |= DIFF_INLINE_WORD;
      } else {
        return FAIL;
      }
    } else if ((strncmp(p, "linematch:", 10) == 0) && ascii_isdigit(p[10])) {
      p += 10;
      linematch_lines_new = getdigits_int(&p, false, linematch_lines_new);
      diff_flags_new |= DIFF_LINEMATCH;

      // linematch does not make sense without filler set
      diff_flags_new |= DIFF_FILLER;
    }

    if ((*p != ',') && (*p != NUL)) {
      return FAIL;
    }

    if (*p == ',') {
      p++;
    }
  }

  diff_algorithm_new |= diff_indent_heuristic;

  // Can't have both "horizontal" and "vertical".
  if ((diff_flags_new & DIFF_HORIZONTAL) && (diff_flags_new & DIFF_VERTICAL)) {
    return FAIL;
  }

  // If flags were added or removed, or the algorithm was changed, need to
  // update the diff.
  if (diff_flags != diff_flags_new || diff_algorithm != diff_algorithm_new) {
    FOR_ALL_TABS(tp) {
      tp->tp_diff_invalid = true;
    }
  }

  diff_flags = diff_flags_new;
  diff_context = diff_context_new == 0 ? 1 : diff_context_new;
  linematch_lines = linematch_lines_new;
  diff_foldcolumn = diff_foldcolumn_new;
  diff_algorithm = diff_algorithm_new;

  diff_redraw(true);

  // recompute the scroll binding with the new option value, may
  // remove or add filler lines
  check_scrollbind(0, 0);
  return OK;
}

/// Check that "diffopt" contains "horizontal".
bool diffopt_horizontal(void)
  FUNC_ATTR_PURE FUNC_ATTR_WARN_UNUSED_RESULT
{
  return (diff_flags & DIFF_HORIZONTAL) != 0;
}

// Return true if 'diffopt' contains "hiddenoff".
bool diffopt_hiddenoff(void)
  FUNC_ATTR_PURE
{
  return (diff_flags & DIFF_HIDDEN_OFF) != 0;
}

// Return true if 'diffopt' contains "closeoff".
bool diffopt_closeoff(void)
  FUNC_ATTR_PURE FUNC_ATTR_WARN_UNUSED_RESULT
{
  return (diff_flags & DIFF_CLOSE_OFF) != 0;
}

// Return true if 'diffopt' contains "filler".
bool diffopt_filler(void)
  FUNC_ATTR_PURE FUNC_ATTR_WARN_UNUSED_RESULT
{
  return (diff_flags & DIFF_FILLER) != 0;
}

/// Called when a line has been updated. Used for updating inline diff in Insert
/// mode without waiting for global diff update later.
void diff_update_line(linenr_T lnum)
{
  if (!(diff_flags & ALL_INLINE_DIFF)) {
    // We only care if we are doing inline-diff where we cache the diff results
    return;
  }

  int idx = diff_buf_idx(curbuf, curtab);
  if (idx == DB_COUNT) {
    return;
  }
  diff_T *dp;
  FOR_ALL_DIFFBLOCKS_IN_TAB(curtab, dp) {
    if (lnum <= dp->df_lnum[idx] + dp->df_count[idx]) {
      break;
    }
  }

  // clear the inline change cache as it's invalid
  if (dp != NULL) {
    dp->has_changes = false;
    dp->df_changes.ga_len = 0;
  }
}

/// used for simple inline diff algorithm
static diffline_change_T simple_diffline_change;

/// Parse a diffline struct and returns the [start,end] byte offsets
///
/// Returns true if this change was added, no other buffer has it.
bool diff_change_parse(diffline_T *diffline, diffline_change_T *change, int *change_start,
                       int *change_end)
{
  if (change->dc_start_lnum_off[diffline->bufidx] < diffline->lineoff) {
    *change_start = 0;
  } else {
    *change_start = change->dc_start[diffline->bufidx];
  }
  if (change->dc_end_lnum_off[diffline->bufidx] > diffline->lineoff) {
    *change_end = INT_MAX;
  } else {
    *change_end = change->dc_end[diffline->bufidx];
  }

  if (change == &simple_diffline_change) {
    // This is what we returned from simple inline diff. We always consider
    // the range to be changed, rather than added for now.
    return false;
  }

  // Find out whether this is an addition. Note that for multi buffer diff,
  // to tell whether lines are additions we check whether all the other diff
  // lines are identical (in diff_check_with_linestatus). If so, we mark them
  // as add. We don't do that for inline diff here for simplicity.
  for (int i = 0; i < DB_COUNT; i++) {
    if (i == diffline->bufidx) {
      continue;
    }
    if (change->dc_start[i] != change->dc_end[i]
        || change->dc_end_lnum_off[i] != change->dc_start_lnum_off[i]) {
      return false;
    }
  }
  return true;
}

/// Find the difference within a changed line and returns [startp,endp] byte
/// positions.  Performs a simple algorithm by finding a single range in the
/// middle.
///
/// If diffopt has DIFF_INLINE_NONE set, then this will only calculate the return
/// value (added or changed), but startp/endp will not be calculated.
///
/// @param  wp      window whose current buffer to check
/// @param  lnum    line number to check within the buffer
/// @param  startp  first char of the change
/// @param  endp    last char of the change
///
/// @return true if the line was added, no other buffer has it.
static bool diff_find_change_simple(win_T *wp, linenr_T lnum, const diff_T *dp, int idx,
                                    int *startp, int *endp)
  FUNC_ATTR_WARN_UNUSED_RESULT FUNC_ATTR_NONNULL_ALL
{
  char *line_org;
  if (diff_flags & DIFF_INLINE_NONE) {
    // We only care about the return value, not the actual string comparisons.
    line_org = NULL;
  } else {
    // Make a copy of the line, the next ml_get() will invalidate it.
    line_org = xstrdup(ml_get_buf(wp->w_buffer, lnum));
  }

  int si_org;
  int si_new;
  int ei_org;
  int ei_new;
  bool added = true;

  linenr_T off = lnum - dp->df_lnum[idx];
  for (int i = 0; i < DB_COUNT; i++) {
    if ((curtab->tp_diffbuf[i] != NULL) && (i != idx)) {
      // Skip lines that are not in the other change (filler lines).
      if (off >= dp->df_count[i]) {
        continue;
      }
      added = false;
      if (diff_flags & DIFF_INLINE_NONE) {
        break;  // early terminate as we only care about the return value
      }

      char *line_new = ml_get_buf(curtab->tp_diffbuf[i], dp->df_lnum[i] + off);

      // Search for start of difference
      si_org = si_new = 0;

      while (line_org[si_org] != NUL) {
        if (((diff_flags & DIFF_IWHITE)
             && ascii_iswhite(line_org[si_org])
             && ascii_iswhite(line_new[si_new]))
            || ((diff_flags & DIFF_IWHITEALL)
                && (ascii_iswhite(line_org[si_org])
                    || ascii_iswhite(line_new[si_new])))) {
          si_org = (int)(skipwhite(line_org + si_org) - line_org);
          si_new = (int)(skipwhite(line_new + si_new) - line_new);
        } else {
          int l;
          if (!diff_equal_char(line_org + si_org, line_new + si_new, &l)) {
            break;
          }
          si_org += l;
          si_new += l;
        }
      }

      // Move back to first byte of character in both lines (may
      // have "nn^" in line_org and "n^ in line_new).
      si_org -= utf_head_off(line_org, line_org + si_org);
      si_new -= utf_head_off(line_new, line_new + si_new);

      *startp = MIN(*startp, si_org);

      // Search for end of difference, if any.
      if ((line_org[si_org] != NUL) || (line_new[si_new] != NUL)) {
        ei_org = (int)strlen(line_org);
        ei_new = (int)strlen(line_new);

        while (ei_org >= *startp
               && ei_new >= si_new
               && ei_org >= 0
               && ei_new >= 0) {
          if (((diff_flags & DIFF_IWHITE)
               && ascii_iswhite(line_org[ei_org])
               && ascii_iswhite(line_new[ei_new]))
              || ((diff_flags & DIFF_IWHITEALL)
                  && (ascii_iswhite(line_org[ei_org])
                      || ascii_iswhite(line_new[ei_new])))) {
            while (ei_org >= *startp && ascii_iswhite(line_org[ei_org])) {
              ei_org--;
            }

            while (ei_new >= si_new && ascii_iswhite(line_new[ei_new])) {
              ei_new--;
            }
          } else {
            const char *p1 = line_org + ei_org;
            const char *p2 = line_new + ei_new;

            p1 -= utf_head_off(line_org, p1);
            p2 -= utf_head_off(line_new, p2);

            int l;
            if (!diff_equal_char(p1, p2, &l)) {
              break;
            }
            ei_org -= l;
            ei_new -= l;
          }
        }

        *endp = MAX(*endp, ei_org);
      }
    }
  }

  xfree(line_org);
  return added;
}

/// Mapping used for mapping from temporary mmfile created for inline diff back
/// to original buffer's line/col.
typedef struct {
  colnr_T byte_start;
  colnr_T num_bytes;
  int lineoff;
} linemap_entry_T;

/// Refine inline character-wise diff blocks to create a more human readable
/// highlight. Otherwise a naive diff under existing algorithms tends to create
/// a messy output with lots of small gaps.
/// It does this by merging adjacent long diff blocks if they are only separated
/// by a couple characters.
/// These are done by heuristics and can be further tuned.
static void diff_refine_inline_char_highlight(diff_T *dp_orig, garray_T *linemap, int idx1)
{
  // Perform multiple passes so that newly merged blocks will now be long
  // enough which may cause other previously unmerged gaps to be merged as
  // well.
  int pass = 1;
  do {
    bool has_unmerged_gaps = false;
    bool has_merged_gaps = false;
    diff_T *dp = dp_orig;
    while (dp != NULL && dp->df_next != NULL) {
      // Only use first buffer to calculate the gap because the gap is
      // unchanged text, which would be the same in all buffers.
      if (dp->df_lnum[idx1] + dp->df_count[idx1] - 1 >= linemap[idx1].ga_len
          || dp->df_next->df_lnum[idx1] - 1 >= linemap[idx1].ga_len) {
        dp = dp->df_next;
        continue;
      }

      // If the gap occurs over different lines, don't consider it
      linemap_entry_T *entry1 =
        &((linemap_entry_T *)linemap[idx1].ga_data)[dp->df_lnum[idx1]
                                                    + dp->df_count[idx1] - 1];
      linemap_entry_T *entry2 =
        &((linemap_entry_T *)linemap[idx1].ga_data)[dp->df_next->df_lnum[idx1] - 1];
      if (entry1->lineoff != entry2->lineoff) {
        dp = dp->df_next;
        continue;
      }

      linenr_T gap = dp->df_next->df_lnum[idx1] - (dp->df_lnum[idx1] + dp->df_count[idx1]);
      if (gap <= 3) {
        linenr_T max_df_count = 0;
        for (int i = 0; i < DB_COUNT; i++) {
          max_df_count = MAX(max_df_count, dp->df_count[i] + dp->df_next->df_count[i]);
        }

        if (max_df_count >= gap * 4) {
          // Merge current block with the next one. Don't advance the
          // pointer so we try the same merged block against the next
          // one.
          for (int i = 0; i < DB_COUNT; i++) {
            dp->df_count[i] = dp->df_next->df_lnum[i]
                              + dp->df_next->df_count[i] - dp->df_lnum[i];
          }
          diff_T *dp_next = dp->df_next;
          dp->df_next = dp_next->df_next;
          clear_diffblock(dp_next);
          has_merged_gaps = true;
          continue;
        } else {
          has_unmerged_gaps = true;
        }
      }
      dp = dp->df_next;
    }
    if (!has_unmerged_gaps || !has_merged_gaps) {
      break;
    }
  } while (pass++ < 4);  // use limited number of passes to avoid excessive looping
}

/// Find the inline difference within a diff block among different buffers.  Do
/// this by splitting each block's content into characters or words, and then
/// use internal xdiff to calculate the per-character/word diff.  The result is
/// stored in dp instead of returned by the function.
static void diff_find_change_inline_diff(diff_T *dp)
{
  const int save_diff_algorithm = diff_algorithm;

  diffio_T dio = { 0 };
  ga_init(&dio.dio_diff.dout_ga, sizeof(diffhunk_T), 1000);

  // inline diff only supports internal algo
  dio.dio_internal = true;

  // always use indent-heuristics to slide diff splits along
  // whitespace
  diff_algorithm |= XDF_INDENT_HEURISTIC;

  // diff_read() has an implicit dependency on curtab->tp_first_diff
  diff_T *orig_diff = curtab->tp_first_diff;
  curtab->tp_first_diff = NULL;

  // diff_read() also uses curtab->tp_diffbuf to determine what's an active
  // buffer
  buf_T *(orig_diffbuf[DB_COUNT]);
  memcpy(orig_diffbuf, curtab->tp_diffbuf, sizeof(orig_diffbuf));

  garray_T linemap[DB_COUNT];
  garray_T file1_str;
  garray_T file2_str;

  // Buffers to populate mmfile 1/2 that would be passed to xdiff as memory
  // files. Use a grow array as it is not obvious how much exact space we
  // need.
  ga_init(&file1_str, 1, 1024);
  ga_init(&file2_str, 1, 1024);

  // Line map to map from generated mmfiles' line numbers back to original
  // diff blocks' locations. Need this even for char diff because not all
  // characters are 1-byte long / ASCII.
  for (int i = 0; i < DB_COUNT; i++) {
    ga_init(&linemap[i], sizeof(linemap_entry_T), 128);
  }

  int file1_idx = -1;
  for (int i = 0; i < DB_COUNT; i++) {
    dio.dio_diff.dout_ga.ga_len = 0;

    buf_T *buf = curtab->tp_diffbuf[i];
    if (buf == NULL || buf->b_ml.ml_mfp == NULL) {
      continue;  // skip buffer that isn't loaded
    }
    if (dp->df_count[i] == 0) {
      // skip buffers that don't have any texts in this block so we don't
      // end up marking the entire block as modified in multi-buffer diff
      curtab->tp_diffbuf[i] = NULL;
      continue;
    }

    if (file1_idx == -1) {
      file1_idx = i;
    }

    garray_T *curstr = (file1_idx != i) ? &file2_str : &file1_str;

    linenr_T numlines = 0;
    curstr->ga_len = 0;

    // Split each line into chars/words and populate fake file buffer as
    // newline-delimited tokens as that's what xdiff requires.
    for (int off = 0; off < dp->df_count[i]; off++) {
      char *curline = ml_get_buf(curtab->tp_diffbuf[i], dp->df_lnum[i] + off);

      bool in_keyword = false;

      // iwhiteeol support vars
      bool last_white = false;
      int eol_ga_len = -1;
      int eol_linemap_len = -1;
      int eol_numlines = -1;

      char *s = curline;
      while (*s != NUL) {
        bool new_in_keyword = false;
        if (diff_flags & DIFF_INLINE_WORD) {
          // Always use the first buffer's 'iskeyword' to have a
          // consistent diff.
          // For multibyte chars, only treat alphanumeric chars
          // (class 2) as "word", as other classes such as emojis and
          // CJK ideographs do not usually benefit from word diff as
          // Vim doesn't have a good way to segment them.
          new_in_keyword = (mb_get_class_tab(s, curtab->tp_diffbuf[file1_idx]->b_chartab) == 2);
        }
        if (in_keyword && !new_in_keyword) {
          ga_append(curstr, NL);
          numlines++;
        }

        if (ascii_iswhite(*s)) {
          if (diff_flags & DIFF_IWHITEALL) {
            in_keyword = false;
            s = skipwhite(s);
            continue;
          } else if ((diff_flags & DIFF_IWHITEEOL) || (diff_flags & DIFF_IWHITE)) {
            if (!last_white) {
              eol_ga_len = curstr->ga_len;
              eol_linemap_len = linemap[i].ga_len;
              eol_numlines = numlines;
              last_white = true;
            }
          }
        } else {
          if ((diff_flags & DIFF_IWHITEEOL) || (diff_flags & DIFF_IWHITE)) {
            last_white = false;
            eol_ga_len = -1;
            eol_linemap_len = -1;
            eol_numlines = -1;
          }
        }

        int char_len = 1;
        if (*s == NL) {
          // NL is internal substitute for NUL
          ga_append(curstr, NUL);
        } else {
          char_len = utfc_ptr2len(s);

          if (ascii_iswhite(*s) && (diff_flags & DIFF_IWHITE)) {
            // Treat the entire white space span as a single char.
            char_len = (int)(skipwhite(s) - s);
          }

          if (diff_flags & DIFF_ICASE) {
            // xdiff doesn't support ignoring case, fold-case the text manually.
            int c = utf_ptr2char(s);
            int c_len = utf_char2len(c);
            c = utf_fold(c);
            char cbuf[MB_MAXBYTES + 1];
            int c_fold_len = utf_char2bytes(c, cbuf);
            ga_concat_len(curstr, cbuf, (size_t)c_fold_len);
            if (char_len > c_len) {
              // There may be remaining composing characters. Write those back in.
              // Composing characters don't need case folding.
              ga_concat_len(curstr, s + c_len, (size_t)(char_len - c_len));
            }
          } else {
            ga_concat_len(curstr, s, (size_t)char_len);
          }
        }

        if (!new_in_keyword) {
          ga_append(curstr, NL);
          numlines++;
        }

        if (!new_in_keyword || (new_in_keyword && !in_keyword)) {
          // create a new mapping entry from the xdiff mmfile back to
          // original line/col.
          linemap_entry_T linemap_entry = {
            .lineoff = off,
            .byte_start = (colnr_T)(s - curline),
            .num_bytes = char_len,
          };
          GA_APPEND(linemap_entry_T, &linemap[i], linemap_entry);
        } else {
          // Still inside a keyword. Just increment byte count but
          // don't make a new entry.
          // linemap always has at least one entry here
          ((linemap_entry_T *)linemap[i].ga_data)[linemap[i].ga_len - 1].num_bytes += char_len;
        }

        in_keyword = new_in_keyword;
        s += char_len;
      }
      if (in_keyword) {
        ga_append(curstr, NL);
        numlines++;
      }

      if ((diff_flags & DIFF_IWHITEEOL) || (diff_flags & DIFF_IWHITE)) {
        // Need to trim trailing whitespace. Do this simply by
        // resetting arrays back to before we encountered them.
        if (eol_ga_len != -1) {
          curstr->ga_len = eol_ga_len;
          linemap[i].ga_len = eol_linemap_len;
          numlines = eol_numlines;
        }
      }

      if (!(diff_flags & DIFF_IWHITEALL)) {
        // Add an empty line token mapped to the end-of-line in the
        // original file. This helps diff newline differences among
        // files, which will be visualized when using 'list' as the eol
        // listchar will be highlighted.
        ga_append(curstr, NL);
        numlines++;

        linemap_entry_T linemap_entry = {
          .lineoff = off,
          .byte_start = (colnr_T)(s - curline),
          .num_bytes = sizeof(NL),
        };
        GA_APPEND(linemap_entry_T, &linemap[i], linemap_entry);
      }
    }

    if (file1_idx != i) {
      dio.dio_new.din_mmfile.ptr = (char *)curstr->ga_data;
      dio.dio_new.din_mmfile.size = curstr->ga_len;
    } else {
      dio.dio_orig.din_mmfile.ptr = (char *)curstr->ga_data;
      dio.dio_orig.din_mmfile.size = curstr->ga_len;
    }
    if (file1_idx != i) {
      // Perform diff with first file and read the results
      int diff_status = diff_file_internal(&dio);
      if (diff_status == FAIL) {
        goto done;
      }

      diff_read(0, i, &dio);
      clear_diffout(&dio.dio_diff);
    }
  }
  diff_T *new_diff = curtab->tp_first_diff;

  if (diff_flags & DIFF_INLINE_CHAR && file1_idx != -1) {
    diff_refine_inline_char_highlight(new_diff, linemap, file1_idx);
  }

  // After the diff, use the linemap to obtain the original line/col of the
  // changes and cache them in dp.
  dp->df_changes.ga_len = 0;  // this should already be zero
  for (; new_diff != NULL; new_diff = new_diff->df_next) {
    diffline_change_T change = { 0 };
    for (int i = 0; i < DB_COUNT; i++) {
      if (new_diff->df_lnum[i] <= 0) {  // should never be < 0. Checking just for safety
        continue;
      }
      linenr_T diff_lnum = new_diff->df_lnum[i] - 1;  // use zero-index
      linenr_T diff_lnum_end = diff_lnum + new_diff->df_count[i];

      if (diff_lnum >= linemap[i].ga_len) {
        change.dc_start[i] = MAXCOL;
        change.dc_start_lnum_off[i] = INT_MAX;
      } else {
        change.dc_start[i] = ((linemap_entry_T *)linemap[i].ga_data)[diff_lnum].byte_start;
        change.dc_start_lnum_off[i] = ((linemap_entry_T *)linemap[i].ga_data)[diff_lnum].lineoff;
      }

      if (diff_lnum == diff_lnum_end) {
        change.dc_end[i] = change.dc_start[i];
        change.dc_end_lnum_off[i] = change.dc_start_lnum_off[i];
      } else if (diff_lnum_end - 1 >= linemap[i].ga_len) {
        change.dc_end[i] = MAXCOL;
        change.dc_end_lnum_off[i] = INT_MAX;
      } else {
        change.dc_end[i] = ((linemap_entry_T *)linemap[i].ga_data)[diff_lnum_end - 1].byte_start +
                           ((linemap_entry_T *)linemap[i].ga_data)[diff_lnum_end - 1].num_bytes;
        change.dc_end_lnum_off[i] = ((linemap_entry_T *)linemap[i].ga_data)[diff_lnum_end -
                                                                            1].lineoff;
      }
    }
    GA_APPEND(diffline_change_T, &dp->df_changes, change);
  }

done:
  diff_algorithm = save_diff_algorithm;

  dp->has_changes = true;

  diff_clear(curtab);
  curtab->tp_first_diff = orig_diff;
  memcpy(curtab->tp_diffbuf, orig_diffbuf, sizeof(orig_diffbuf));

  ga_clear(&file1_str);
  ga_clear(&file2_str);
  // No need to clear dio.dio_orig/dio_new because they were referencing
  // strings that are now cleared.
  clear_diffout(&dio.dio_diff);
  for (int i = 0; i < DB_COUNT; i++) {
    ga_clear(&linemap[i]);
  }
}

/// Find the difference within a changed line.
/// Returns true if the line was added, no other buffer has it.
bool diff_find_change(win_T *wp, linenr_T lnum, diffline_T *diffline)
  FUNC_ATTR_WARN_UNUSED_RESULT FUNC_ATTR_NONNULL_ALL
{
  int idx = diff_buf_idx(wp->w_buffer, curtab);
  if (idx == DB_COUNT) {  // cannot happen
    return false;
  }

  // search for a change that includes "lnum" in the list of diffblocks.
  diff_T *dp;
  FOR_ALL_DIFFBLOCKS_IN_TAB(curtab, dp) {
    if (lnum < dp->df_lnum[idx] + dp->df_count[idx]) {
      break;
    }
  }
  if (dp == NULL || diff_check_sanity(curtab, dp) == FAIL) {
    return false;
  }

  int off = lnum - dp->df_lnum[idx];

  if (!(diff_flags & ALL_INLINE_DIFF)) {
    // Use simple algorithm
    int change_start = MAXCOL;  // first col of changed area
    int change_end = -1;        // last col of changed area

    int ret = diff_find_change_simple(wp, lnum, dp, idx, &change_start, &change_end);

    // convert from inclusive end to exclusive end per diffline's contract
    change_end += 1;

    // Create a mock diffline struct. We always only have one so no need to
    // allocate memory.
    CLEAR_FIELD(simple_diffline_change);
    diffline->changes = &simple_diffline_change;
    diffline->num_changes = 1;
    diffline->bufidx = idx;
    diffline->lineoff = lnum - dp->df_lnum[idx];

    simple_diffline_change.dc_start[idx] = change_start;
    simple_diffline_change.dc_end[idx] = change_end;
    simple_diffline_change.dc_start_lnum_off[idx] = off;
    simple_diffline_change.dc_end_lnum_off[idx] = off;
    return ret;
  }

  // Use inline diff algorithm.
  // The diff changes are usually cached so we check that first.
  if (!dp->has_changes) {
    diff_find_change_inline_diff(dp);
  }

  garray_T *changes = &dp->df_changes;

  // Use linear search to find the first change for this line. We could
  // optimize this to use binary search, but there should usually be a
  // limited number of inline changes per diff block, and limited number of
  // diff blocks shown on screen, so it is not necessary.
  int num_changes = 0;
  int change_idx = 0;
  diffline->changes = NULL;
  for (change_idx = 0; change_idx < changes->ga_len; change_idx++) {
    diffline_change_T *change =
      &((diffline_change_T *)dp->df_changes.ga_data)[change_idx];
    if (change->dc_end_lnum_off[idx] < off) {
      continue;
    }
    if (change->dc_start_lnum_off[idx] > off) {
      break;
    }
    if (diffline->changes == NULL) {
      diffline->changes = change;
    }
    num_changes++;
  }
  diffline->num_changes = num_changes;
  diffline->bufidx = idx;
  diffline->lineoff = off;

  // Detect simple cases of added lines in the end within a diff block. This
  // has to be the last change of this diff block, and all other buffers are
  // considering this to be an addition past their last line. Other scenarios
  // will be considered a changed line instead.
  bool added = false;
  if (num_changes == 1 && change_idx == dp->df_changes.ga_len) {
    added = true;
    for (int i = 0; i < DB_COUNT; i++) {
      if (idx == i) {
        continue;
      }
      if (curtab->tp_diffbuf[i] == NULL) {
        continue;
      }
      diffline_change_T *change =
        &((diffline_change_T *)dp->df_changes.ga_data)[dp->df_changes.ga_len - 1];
      if (change->dc_start_lnum_off[i] != INT_MAX) {
        added = false;
        break;
      }
    }
  }
  return added;
}

/// Check that line "lnum" is not close to a diff block, this line should
/// be in a fold.
///
/// @param  wp    window containing the buffer to check
/// @param  lnum  line number to check within the buffer
///
/// @return false if there are no diff blocks at all in this window.
bool diff_infold(win_T *wp, linenr_T lnum)
  FUNC_ATTR_PURE FUNC_ATTR_WARN_UNUSED_RESULT FUNC_ATTR_NONNULL_ARG(1)
{
  // Return if 'diff' isn't set.
  if (!wp->w_p_diff) {
    return false;
  }

  int idx = -1;
  bool other = false;
  for (int i = 0; i < DB_COUNT; i++) {
    if (curtab->tp_diffbuf[i] == wp->w_buffer) {
      idx = i;
    } else if (curtab->tp_diffbuf[i] != NULL) {
      other = true;
    }
  }

  // return here if there are no diffs in the window
  if ((idx == -1) || !other) {
    return false;
  }

  if (curtab->tp_diff_invalid) {
    // update after a big change
    ex_diffupdate(NULL);
  }

  // Return if there are no diff blocks.  All lines will be folded.
  if (curtab->tp_first_diff == NULL) {
    return true;
  }

  for (diff_T *dp = curtab->tp_first_diff; dp != NULL; dp = dp->df_next) {
    // If this change is below the line there can't be any further match.
    if (dp->df_lnum[idx] - diff_context > lnum) {
      break;
    }

    // If this change ends before the line we have a match.
    if (dp->df_lnum[idx] + dp->df_count[idx] + diff_context > lnum) {
      return false;
    }
  }
  return true;
}

/// "dp" and "do" commands.
void nv_diffgetput(bool put, size_t count)
{
  if (bt_prompt(curbuf)) {
    vim_beep(kOptBoFlagOperator);
    return;
  }

  exarg_T ea;
  char buf[30];
  if (count == 0) {
    ea.arg = "";
  } else {
    vim_snprintf(buf, sizeof(buf), "%zu", count);
    ea.arg = buf;
  }

  if (put) {
    ea.cmdidx = CMD_diffput;
  } else {
    ea.cmdidx = CMD_diffget;
  }

  ea.addr_count = 0;
  ea.line1 = curwin->w_cursor.lnum;
  ea.line2 = curwin->w_cursor.lnum;
  ex_diffgetput(&ea);
}

/// Return true if "diff" appears in the list of diff blocks of the current tab.
static bool valid_diff(diff_T *diff)
{
  for (diff_T *dp = curtab->tp_first_diff; dp != NULL; dp = dp->df_next) {
    if (dp == diff) {
      return true;
    }
  }
  return false;
}

/// ":diffget" and ":diffput"
///
/// @param eap
void ex_diffgetput(exarg_T *eap)
{
  int idx_other;

  // Find the current buffer in the list of diff buffers.
  int idx_cur = diff_buf_idx(curbuf, curtab);
  if (idx_cur == DB_COUNT) {
    emsg(_("E99: Current buffer is not in diff mode"));
    return;
  }

  if (*eap->arg == NUL) {
    bool found_not_ma = false;
    // No argument: Find the other buffer in the list of diff buffers.
    for (idx_other = 0; idx_other < DB_COUNT; idx_other++) {
      if ((curtab->tp_diffbuf[idx_other] != curbuf)
          && (curtab->tp_diffbuf[idx_other] != NULL)) {
        if ((eap->cmdidx != CMD_diffput)
            || MODIFIABLE(curtab->tp_diffbuf[idx_other])) {
          break;
        }
        found_not_ma = true;
      }
    }

    if (idx_other == DB_COUNT) {
      if (found_not_ma) {
        emsg(_("E793: No other buffer in diff mode is modifiable"));
      } else {
        emsg(_("E100: No other buffer in diff mode"));
      }
      return;
    }

    // Check that there isn't a third buffer in the list
    for (int i = idx_other + 1; i < DB_COUNT; i++) {
      if ((curtab->tp_diffbuf[i] != curbuf)
          && (curtab->tp_diffbuf[i] != NULL)
          && ((eap->cmdidx != CMD_diffput)
              || MODIFIABLE(curtab->tp_diffbuf[i]))) {
        emsg(_("E101: More than two buffers in diff mode, don't know "
               "which one to use"));
        return;
      }
    }
  } else {
    // Buffer number or pattern given. Ignore trailing white space.
    char *p = eap->arg + strlen(eap->arg);
    while (p > eap->arg && ascii_iswhite(p[-1])) {
      p--;
    }

    int i;
    for (i = 0; ascii_isdigit(eap->arg[i]) && eap->arg + i < p; i++) {}

    if (eap->arg + i == p) {
      // digits only
      i = (int)atol(eap->arg);
    } else {
      i = buflist_findpat(eap->arg, p, false, true, false);

      if (i < 0) {
        // error message already given
        return;
      }
    }
    buf_T *buf = buflist_findnr(i);

    if (buf == NULL) {
      semsg(_("E102: Can't find buffer \"%s\""), eap->arg);
      return;
    }

    if (buf == curbuf) {
      // nothing to do
      return;
    }
    idx_other = diff_buf_idx(buf, curtab);

    if (idx_other == DB_COUNT) {
      semsg(_("E103: Buffer \"%s\" is not in diff mode"), eap->arg);
      return;
    }
  }

  diff_busy = true;

  // When no range given include the line above or below the cursor.
  if (eap->addr_count == 0) {
    // Make it possible that ":diffget" on the last line gets line below
    // the cursor line when there is no difference above the cursor.
    int linestatus = 0;
    if (eap->line1 == curbuf->b_ml.ml_line_count
        && (diff_check_with_linestatus(curwin, eap->line1, &linestatus) == 0
            && linestatus == 0)
        && (eap->line1 == 1
            || (diff_check_with_linestatus(curwin, eap->line1 - 1, &linestatus) >= 0
                && linestatus == 0))) {
      eap->line2++;
    } else if (eap->line1 > 0) {
      eap->line1--;
    }
  }

  aco_save_T aco;

  if (eap->cmdidx != CMD_diffget) {
    // Need to make the other buffer the current buffer to be able to make
    // changes in it.

    // Set curwin/curbuf to buf and save a few things.
    aucmd_prepbuf(&aco, curtab->tp_diffbuf[idx_other]);
  }

  const int idx_from = eap->cmdidx == CMD_diffget ? idx_other : idx_cur;
  const int idx_to = eap->cmdidx == CMD_diffget ? idx_cur : idx_other;

  // May give the warning for a changed buffer here, which can trigger the
  // FileChangedRO autocommand, which may do nasty things and mess
  // everything up.
  if (!curbuf->b_changed) {
    change_warning(curbuf, 0);
    if (diff_buf_idx(curbuf, curtab) != idx_to) {
      emsg(_("E787: Buffer changed unexpectedly"));
      goto theend;
    }
  }

  diffgetput(eap->addr_count, idx_cur, idx_from, idx_to, eap->line1, eap->line2);

  // restore curwin/curbuf and a few other things
  if (eap->cmdidx != CMD_diffget) {
    // Syncing undo only works for the current buffer, but we change
    // another buffer.  Sync undo if the command was typed.  This isn't
    // 100% right when ":diffput" is used in a function or mapping.
    if (KeyTyped) {
      u_sync(false);
    }
    aucmd_restbuf(&aco);
  }

theend:
  diff_busy = false;

  if (diff_need_update) {
    ex_diffupdate(NULL);
  }

  // Check that the cursor is on a valid character and update its
  // position.  When there were filler lines the topline has become
  // invalid.
  check_cursor(curwin);
  changed_line_abv_curs();

  // If all diffs are gone, update folds in all diff windows.
  if (curtab->tp_first_diff == NULL) {
    FOR_ALL_WINDOWS_IN_TAB(wp, curtab) {
      if (wp->w_p_diff && wp->w_p_fdm[0] == 'd' && wp->w_p_fen) {
        foldUpdateAll(wp);
      }
    }
  }

  if (diff_need_update) {
    // redraw already done by ex_diffupdate()
    diff_need_update = false;
  } else {
    // Also need to redraw the other buffers.
    diff_redraw(false);
    apply_autocmds(EVENT_DIFFUPDATED, NULL, NULL, false, curbuf);
  }
}

/// Apply diffget/diffput to buffers and diffblocks
///
/// @param idx_cur   index of "curbuf" before aucmd_prepbuf() in the list of diff buffers
/// @param idx_from  index of the buffer to read from in the list of diff buffers
/// @param idx_to    index of the buffer to modify in the list of diff buffers
static void diffgetput(const int addr_count, const int idx_cur, const int idx_from,
                       const int idx_to, const linenr_T line1, const linenr_T line2)
{
  linenr_T off = 0;
  diff_T *dprev = NULL;

  for (diff_T *dp = curtab->tp_first_diff; dp != NULL;) {
    if (!addr_count) {
      // Handle the case with adjacent diff blocks (e.g. using linematch
      // or anchors) at/above the cursor. Since a range wasn't specified,
      // we just want to grab one diff block rather than all of them in
      // the vicinity.
      while (dp->df_next
             && dp->df_next->df_lnum[idx_cur] == dp->df_lnum[idx_cur] + dp->df_count[idx_cur]
             && dp->df_next->df_lnum[idx_cur] == line1 + off + 1) {
        dprev = dp;
        dp = dp->df_next;
      }
    }

    if (dp->df_lnum[idx_cur] > line2 + off) {
      // past the range that was specified
      break;
    }
    diff_T dfree = { 0 };
    bool did_free = false;
    linenr_T lnum = dp->df_lnum[idx_to];
    linenr_T count = dp->df_count[idx_to];

    if ((dp->df_lnum[idx_cur] + dp->df_count[idx_cur] > line1 + off)
        && (u_save(lnum - 1, lnum + count) != FAIL)) {
      // Inside the specified range and saving for undo worked.
      linenr_T start_skip = 0;
      linenr_T end_skip = 0;

      if (addr_count > 0) {
        // A range was specified: check if lines need to be skipped.
        start_skip = line1 + off - dp->df_lnum[idx_cur];
        if (start_skip > 0) {
          // range starts below start of current diff block
          if (start_skip > count) {
            lnum += count;
            count = 0;
          } else {
            count -= start_skip;
            lnum += start_skip;
          }
        } else {
          start_skip = 0;
        }

        end_skip = dp->df_lnum[idx_cur] + dp->df_count[idx_cur] - 1
                   - (line2 + off);

        if (end_skip > 0) {
          // range ends above end of current/from diff block
          if (idx_cur == idx_from) {
            // :diffput
            count = MIN(count, dp->df_count[idx_cur] - start_skip - end_skip);
          } else {
            // :diffget
            count -= end_skip;
            end_skip = MAX(dp->df_count[idx_from] - start_skip - count, 0);
          }
        } else {
          end_skip = 0;
        }
      }

      bool buf_empty = buf_is_empty(curbuf);
      int added = 0;

      for (int i = 0; i < count; i++) {
        // remember deleting the last line of the buffer
        buf_empty = curbuf->b_ml.ml_line_count == 1;
        if (ml_delete(lnum, false) == OK) {
          added--;
        }
      }

      for (int i = 0; i < dp->df_count[idx_from] - start_skip - end_skip; i++) {
        linenr_T nr = dp->df_lnum[idx_from] + start_skip + i;
        if (nr > curtab->tp_diffbuf[idx_from]->b_ml.ml_line_count) {
          break;
        }
        char *p = xstrdup(ml_get_buf(curtab->tp_diffbuf[idx_from], nr));
        ml_append(lnum + i - 1, p, 0, false);
        xfree(p);
        added++;
        if (buf_empty && (curbuf->b_ml.ml_line_count == 2)) {
          // Added the first line into an empty buffer, need to
          // delete the dummy empty line.
          // This has a side effect of incrementing curbuf->deleted_bytes,
          // which results in inaccurate reporting of the byte count of
          // previous contents in buffer-update events.
          buf_empty = false;
          ml_delete(2, false);
        }
      }
      linenr_T new_count = dp->df_count[idx_to] + added;
      dp->df_count[idx_to] = new_count;

      if ((start_skip == 0) && (end_skip == 0)) {
        // Check if there are any other buffers and if the diff is
        // equal in them.
        int i;
        for (i = 0; i < DB_COUNT; i++) {
          if ((curtab->tp_diffbuf[i] != NULL)
              && (i != idx_from)
              && (i != idx_to)
              && !diff_equal_entry(dp, idx_from, i)) {
            break;
          }
        }

        if (i == DB_COUNT) {
          // delete the diff entry, the buffers are now equal here
          dfree = *dp;
          did_free = true;
          dp = diff_free(curtab, dprev, dp);
        }
      }

      if (added != 0) {
        // Adjust marks.  This will change the following entries!
        mark_adjust(lnum, lnum + count - 1, MAXLNUM, added, kExtmarkNOOP);
        if (curwin->w_cursor.lnum >= lnum) {
          // Adjust the cursor position if it's in/after the changed
          // lines.
          if (curwin->w_cursor.lnum >= lnum + count) {
            curwin->w_cursor.lnum += added;
          } else if (added < 0) {
            curwin->w_cursor.lnum = lnum;
          }
        }
      }
      extmark_adjust(curbuf, lnum, lnum + count - 1, MAXLNUM, added, kExtmarkUndo);
      changed_lines(curbuf, lnum, 0, lnum + count, added, true);

      if (did_free) {
        // Diff is deleted, update folds in other windows.
        diff_fold_update(&dfree, idx_to);
      }

      // mark_adjust() may have made "dp" invalid.  We don't know where
      // to continue then, bail out.
      if (added != 0 && !valid_diff(dp)) {
        break;
      }

      if (!did_free) {
        // mark_adjust() may have changed the count in a wrong way
        dp->df_count[idx_to] = new_count;
      }

      // When changing the current buffer, keep track of line numbers
      if (idx_cur == idx_to) {
        off += added;
      }
    }

    // If before the range or not deleted, go to next diff.
    if (!did_free) {
      dprev = dp;
      dp = dp->df_next;
    }
  }
}

/// Update folds for all diff buffers for entry "dp".
///
/// Skip buffer with index "skip_idx".
/// When there are no diffs, all folds are removed.
///
/// @param dp
/// @param skip_idx
static void diff_fold_update(diff_T *dp, int skip_idx)
{
  FOR_ALL_WINDOWS_IN_TAB(wp, curtab) {
    for (int i = 0; i < DB_COUNT; i++) {
      if ((curtab->tp_diffbuf[i] == wp->w_buffer) && (i != skip_idx)) {
        foldUpdate(wp, dp->df_lnum[i], dp->df_lnum[i] + dp->df_count[i]);
      }
    }
  }
}

/// Checks that the buffer is in diff-mode.
///
/// @param  buf  buffer to check.
bool diff_mode_buf(buf_T *buf)
  FUNC_ATTR_PURE FUNC_ATTR_WARN_UNUSED_RESULT FUNC_ATTR_NONNULL_ARG(1)
{
  FOR_ALL_TABS(tp) {
    if (diff_buf_idx(buf, tp) != DB_COUNT) {
      return true;
    }
  }
  return false;
}

/// Move "count" times in direction "dir" to the next diff block.
///
/// @param dir
/// @param count
///
/// @return FAIL if there isn't such a diff block.
int diff_move_to(int dir, int count)
{
  linenr_T lnum = curwin->w_cursor.lnum;
  int idx = diff_buf_idx(curbuf, curtab);
  if ((idx == DB_COUNT) || (curtab->tp_first_diff == NULL)) {
    return FAIL;
  }

  if (curtab->tp_diff_invalid) {
    // update after a big change
    ex_diffupdate(NULL);
  }

  if (curtab->tp_first_diff == NULL) {
    // no diffs today
    return FAIL;
  }

  while (--count >= 0) {
    // Check if already before first diff.
    if ((dir == BACKWARD) && (lnum <= curtab->tp_first_diff->df_lnum[idx])) {
      break;
    }

    diff_T *dp;
    for (dp = curtab->tp_first_diff;; dp = dp->df_next) {
      if (dp == NULL) {
        break;
      }

      if (((dir == FORWARD) && (lnum < dp->df_lnum[idx]))
          || ((dir == BACKWARD)
              && ((dp->df_next == NULL)
                  || (lnum <= dp->df_next->df_lnum[idx])))) {
        lnum = dp->df_lnum[idx];
        break;
      }
    }
  }

  // don't end up past the end of the file
  lnum = MIN(lnum, curbuf->b_ml.ml_line_count);

  // When the cursor didn't move at all we fail.
  if (lnum == curwin->w_cursor.lnum) {
    return FAIL;
  }

  setpcmark();
  curwin->w_cursor.lnum = lnum;
  curwin->w_cursor.col = 0;

  return OK;
}

/// Return the line number in the current window that is closest to "lnum1" in
/// "buf1" in diff mode.
static linenr_T diff_get_corresponding_line_int(buf_T *buf1, linenr_T lnum1)
{
  linenr_T baseline = 0;

  int idx1 = diff_buf_idx(buf1, curtab);
  int idx2 = diff_buf_idx(curbuf, curtab);

  if ((idx1 == DB_COUNT)
      || (idx2 == DB_COUNT)
      || (curtab->tp_first_diff == NULL)) {
    return lnum1;
  }

  if (curtab->tp_diff_invalid) {
    // update after a big change
    ex_diffupdate(NULL);
  }

  if (curtab->tp_first_diff == NULL) {
    // no diffs today
    return lnum1;
  }

  for (diff_T *dp = curtab->tp_first_diff; dp != NULL; dp = dp->df_next) {
    if (dp->df_lnum[idx1] > lnum1) {
      return lnum1 - baseline;
    }
    if ((dp->df_lnum[idx1] + dp->df_count[idx1]) > lnum1) {
      // Inside the diffblock
      baseline = lnum1 - dp->df_lnum[idx1];
      baseline = MIN(baseline, dp->df_count[idx2]);

      return dp->df_lnum[idx2] + baseline;
    }
    if ((dp->df_lnum[idx1] == lnum1)
        && (dp->df_count[idx1] == 0)
        && (dp->df_lnum[idx2] <= curwin->w_cursor.lnum)
        && ((dp->df_lnum[idx2] + dp->df_count[idx2])
            > curwin->w_cursor.lnum)) {
      // Special case: if the cursor is just after a zero-count
      // block (i.e. all filler) and the target cursor is already
      // inside the corresponding block, leave the target cursor
      // unmoved. This makes repeated CTRL-W W operations work
      // as expected.
      return curwin->w_cursor.lnum;
    }
    baseline = (dp->df_lnum[idx1] + dp->df_count[idx1])
               - (dp->df_lnum[idx2] + dp->df_count[idx2]);
  }

  // If we get here then the cursor is after the last diff
  return lnum1 - baseline;
}

/// Finds the corresponding line in a diff.
///
/// @param buf1
/// @param lnum1
///
/// @return The corresponding line.
linenr_T diff_get_corresponding_line(buf_T *buf1, linenr_T lnum1)
{
  linenr_T lnum = diff_get_corresponding_line_int(buf1, lnum1);

  // don't end up past the end of the file
  return MIN(lnum, curbuf->b_ml.ml_line_count);
}

/// For line "lnum" in the current window find the equivalent lnum in window
/// "wp", compensating for inserted/deleted lines.
linenr_T diff_lnum_win(linenr_T lnum, win_T *wp)
{
  diff_T *dp;

  int idx = diff_buf_idx(curbuf, curtab);

  if (idx == DB_COUNT) {
    // safety check
    return 0;
  }

  if (curtab->tp_diff_invalid) {
    // update after a big change
    ex_diffupdate(NULL);
  }

  // search for a change that includes "lnum" in the list of diffblocks.
  for (dp = curtab->tp_first_diff; dp != NULL; dp = dp->df_next) {
    if (lnum <= dp->df_lnum[idx] + dp->df_count[idx]) {
      break;
    }
  }

  // When after the last change, compute relative to the last line number.
  if (dp == NULL) {
    return wp->w_buffer->b_ml.ml_line_count
           - (curbuf->b_ml.ml_line_count - lnum);
  }

  // Find index for "wp".
  int i = diff_buf_idx(wp->w_buffer, curtab);

  if (i == DB_COUNT) {
    // safety check
    return 0;
  }

  linenr_T n = lnum + (dp->df_lnum[i] - dp->df_lnum[idx]);
  return MIN(n, dp->df_lnum[i] + dp->df_count[i]);
}

/// Handle an ED style diff line.
///
/// @return  FAIL if the line does not contain diff info.
static int parse_diff_ed(char *line, diffhunk_T *hunk)
{
  int l1, l2;

  // The line must be one of three formats:
  // change: {first}[,{last}]c{first}[,{last}]
  // append: {first}a{first}[,{last}]
  // delete: {first}[,{last}]d{first}
  char *p = line;
  linenr_T f1 = getdigits_int32(&p, true, 0);
  if (*p == ',') {
    p++;
    l1 = getdigits_int(&p, true, 0);
  } else {
    l1 = f1;
  }
  if (*p != 'a' && *p != 'c' && *p != 'd') {
    return FAIL;        // invalid diff format
  }
  int difftype = (uint8_t)(*p++);
  int f2 = getdigits_int(&p, true, 0);
  if (*p == ',') {
    p++;
    l2 = getdigits_int(&p, true, 0);
  } else {
    l2 = f2;
  }
  if (l1 < f1 || l2 < f2) {
    return FAIL;
  }

  if (difftype == 'a') {
    hunk->lnum_orig = f1 + 1;
    hunk->count_orig = 0;
  } else {
    hunk->lnum_orig = f1;
    hunk->count_orig = l1 - f1 + 1;
  }
  if (difftype == 'd') {
    hunk->lnum_new = (linenr_T)f2 + 1;
    hunk->count_new = 0;
  } else {
    hunk->lnum_new = (linenr_T)f2;
    hunk->count_new = l2 - f2 + 1;
  }
  return OK;
}

/// Parses unified diff with zero(!) context lines.
/// Return FAIL if there is no diff information in "line".
static int parse_diff_unified(char *line, diffhunk_T *hunk)
{
  // Parse unified diff hunk header:
  // @@ -oldline,oldcount +newline,newcount @@
  char *p = line;
  if (*p++ == '@' && *p++ == '@' && *p++ == ' ' && *p++ == '-') {
    int oldcount;
    linenr_T newline;
    int newcount;
    linenr_T oldline = getdigits_int32(&p, true, 0);
    if (*p == ',') {
      p++;
      oldcount = getdigits_int(&p, true, 0);
    } else {
      oldcount = 1;
    }
    if (*p++ == ' ' && *p++ == '+') {
      newline = getdigits_int(&p, true, 0);
      if (*p == ',') {
        p++;
        newcount = getdigits_int(&p, true, 0);
      } else {
        newcount = 1;
      }
    } else {
      return FAIL;  // invalid diff format
    }

    if (oldcount == 0) {
      oldline += 1;
    }
    if (newcount == 0) {
      newline += 1;
    }
    if (newline == 0) {
      newline = 1;
    }

    hunk->lnum_orig = oldline;
    hunk->count_orig = oldcount;
    hunk->lnum_new = newline;
    hunk->count_new = newcount;

    return OK;
  }

  return FAIL;
}

/// Callback function for the xdl_diff() function.
/// Stores the diff output in a grow array.
static int xdiff_out(int start_a, int count_a, int start_b, int count_b, void *priv)
{
  diffout_T *dout = (diffout_T *)priv;
  GA_APPEND(diffhunk_T, &(dout->dout_ga), ((diffhunk_T){
    .lnum_orig = (linenr_T)start_a + 1,
    .count_orig = count_a,
    .lnum_new = (linenr_T)start_b + 1,
    .count_new = count_b,
  }));
  return 0;
}

/// "diff_filler()" function
void f_diff_filler(typval_T *argvars, typval_T *rettv, EvalFuncData fptr)
{
  rettv->vval.v_number = MAX(0, diff_check_fill(curwin, tv_get_lnum(argvars)));
}

/// "diff_hlID()" function
void f_diff_hlID(typval_T *argvars, typval_T *rettv, EvalFuncData fptr)
{
  static linenr_T prev_lnum = 0;
  static varnumber_T changedtick = 0;
  static int fnum = 0;
  static int prev_diff_flags = 0;
  static int change_start = 0;
  static int change_end = 0;
  static hlf_T hlID = (hlf_T)0;

  diffline_T diffline = { 0 };
  // Remember the results if using simple since it's recalculated per
  // call. Otherwise just call diff_find_change() every time since
  // internally the result is cached internally.
  const bool cache_results = !(diff_flags & ALL_INLINE_DIFF);

  linenr_T lnum = tv_get_lnum(argvars);
  if (lnum < 0) {       // ignore type error in {lnum} arg
    lnum = 0;
  }
  if (!cache_results
      || lnum != prev_lnum
      || changedtick != buf_get_changedtick(curbuf)
      || fnum != curbuf->b_fnum
      || diff_flags != prev_diff_flags) {
    // New line, buffer, change: need to get the values.
    int linestatus = 0;
    diff_check_with_linestatus(curwin, lnum, &linestatus);
    if (linestatus < 0) {
      if (linestatus == -1) {
        change_start = MAXCOL;
        change_end = -1;
        if (diff_find_change(curwin, lnum, &diffline)) {
          hlID = HLF_ADD;               // added line
        } else {
          hlID = HLF_CHD;               // changed line
          if (diffline.num_changes > 0 && cache_results) {
            change_start = diffline.changes[0].dc_start[diffline.bufidx];
            change_end = diffline.changes[0].dc_end[diffline.bufidx];
          }
        }
      } else {
        hlID = HLF_ADD;         // added line
      }
    } else {
      hlID = (hlf_T)0;
    }

    if (cache_results) {
      prev_lnum = lnum;
      changedtick = buf_get_changedtick(curbuf);
      fnum = curbuf->b_fnum;
      prev_diff_flags = diff_flags;
    }
  }

  if (hlID == HLF_CHD || hlID == HLF_TXD) {
    int col = (int)tv_get_number(&argvars[1]) - 1;  // Ignore type error in {col}.
    if (cache_results) {
      if (col >= change_start && col < change_end) {
        hlID = HLF_TXD;  // Changed text.
      } else {
        hlID = HLF_CHD;  // Changed line.
      }
    } else {
      hlID = HLF_CHD;
      for (int i = 0; i < diffline.num_changes; i++) {
        bool added = diff_change_parse(&diffline, &diffline.changes[i],
                                       &change_start, &change_end);
        if (col >= change_start && col < change_end) {
          hlID = added ? HLF_TXA : HLF_TXD;
          break;
        }
        if (col < change_start) {
          // the remaining changes are past this column and not relevant
          break;
        }
      }
    }
  }
  rettv->vval.v_number = hlID;
}
