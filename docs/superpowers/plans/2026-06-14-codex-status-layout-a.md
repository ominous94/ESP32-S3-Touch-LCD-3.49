# Codex Status Layout A Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rebuild the `Examples/Arduino/12_Codex_Status` LVGL screen as the approved Layout A: left companion anchor, center primary session detail, and right secondary session cards.

**Architecture:** Keep the existing Wi-Fi, HTTP polling, JSON parsing, status sorting, image mapping, SD TTF fallback, and serial telemetry. Replace the current left-list/right-large-character UI object tree with three fixed-width LVGL columns and update `update_status_ui()` to bind the highest-priority session to the primary detail area and up to two following sessions to secondary cards.

**Tech Stack:** Arduino ESP32, LVGL 9, Python `unittest` static checks, repo verification scripts.

---

### File Structure

- Modify `tests/test_codex_status_images.py`: update static assertions from the old right-side character layout to Layout A object names, constants, and text behavior.
- Modify `Examples/Arduino/12_Codex_Status/12_Codex_Status.ino`: replace UI constants, object handles, `create_status_ui()`, and `update_status_ui()` rendering logic.
- Keep `tools/codex_status_bridge.py` and `tools/export_codex_sessions.py` unchanged because the approved spec does not expand the JSON protocol.
- Keep generated image assets unchanged unless compile errors prove image dimensions must be regenerated.

### Task 1: Add Static Tests for Layout A

**Files:**
- Modify: `tests/test_codex_status_images.py`
- Test: `python -m unittest tests.test_codex_status_images -v`

- [ ] **Step 1: Write the failing tests**

Update the layout test to assert the sketch contains these Layout A names and no longer depends on the old large right panel:

```python
    def test_sketch_uses_layout_a_companion_primary_secondary_columns(self):
        sketch = SKETCH_FILE.read_text(encoding="utf-8")
        config = CONFIG_FILE.read_text(encoding="utf-8")

        self.assertIn("#define Rotated USER_DISP_ROT_90", config)
        self.assertIn("COMPANION_PANEL_W", sketch)
        self.assertIn("PRIMARY_PANEL_W", sketch)
        self.assertIn("SECONDARY_PANEL_W", sketch)
        self.assertIn("primary_title_label", sketch)
        self.assertIn("primary_next_label", sketch)
        self.assertIn("primary_verify_label", sketch)
        self.assertIn("secondary_cards", sketch)
        self.assertIn("assistant_image", sketch)
        self.assertNotIn("RIGHT_PANEL_W", sketch)
```

Add a focused behavior-shape test for the new primary session helpers:

```python
    def test_layout_a_derives_primary_session_status_details(self):
        sketch = SKETCH_FILE.read_text(encoding="utf-8")

        self.assertIn("static String next_step_text", sketch)
        self.assertIn("static int state_progress_width", sketch)
        self.assertIn("下一步：继续处理当前任务", sketch)
        self.assertIn("验证：编译 + 真机日志", sketch)
        self.assertIn("暂无会话", sketch)
```

Replace the two-line row layout assertion with fixed non-scroll primary/secondary assertions:

```python
    def test_layout_a_text_containers_are_fixed_and_non_scrollable(self):
        sketch = SKETCH_FILE.read_text(encoding="utf-8")

        self.assertIn("PRIMARY_TITLE_W", sketch)
        self.assertIn("SECONDARY_TITLE_W", sketch)
        self.assertIn("lv_obj_clear_flag(primary_panel, LV_OBJ_FLAG_SCROLLABLE)", sketch)
        self.assertIn("lv_obj_clear_flag(secondary_cards[i], LV_OBJ_FLAG_SCROLLABLE)", sketch)
        self.assertIn("lv_label_set_long_mode(primary_title_label, LV_LABEL_LONG_DOT)", sketch)
        self.assertIn("lv_label_set_long_mode(secondary_titles[i], LV_LABEL_LONG_DOT)", sketch)
        self.assertNotIn("lv_obj_set_height(session_rows[i], SESSION_ROW_H)", sketch)
```

- [ ] **Step 2: Run the focused test and verify RED**

Run:

```powershell
python -m unittest tests.test_codex_status_images -v
```

Expected: FAIL because `COMPANION_PANEL_W`, `PRIMARY_PANEL_W`, `primary_title_label`, and the new helper functions do not exist yet.

### Task 2: Implement Layout A UI Object Tree

**Files:**
- Modify: `Examples/Arduino/12_Codex_Status/12_Codex_Status.ino`
- Test: `python -m unittest tests.test_codex_status_images -v`

- [ ] **Step 1: Replace old layout constants and object handles**

Use these constants near the existing `VISIBLE_SESSIONS` declaration:

```cpp
static const int SECONDARY_SESSIONS = 2;
static const int COMPANION_PANEL_W = 150;
static const int PRIMARY_PANEL_W = 318;
static const int SECONDARY_PANEL_W = 148;
static const int PANEL_H = 160;
static const int COMPANION_IMAGE_W = 58;
static const int COMPANION_IMAGE_H = 96;
static const int PRIMARY_TITLE_W = 286;
static const int SECONDARY_TITLE_W = 124;
static const int SECONDARY_CARD_H = 74;
```

Replace the old session row handles with:

```cpp
static lv_obj_t *companion_state_label = NULL;
static lv_obj_t *companion_summary_label = NULL;
static lv_obj_t *primary_status_dot = NULL;
static lv_obj_t *primary_status_label = NULL;
static lv_obj_t *primary_title_label = NULL;
static lv_obj_t *primary_meta_label = NULL;
static lv_obj_t *primary_progress_bar = NULL;
static lv_obj_t *primary_next_label = NULL;
static lv_obj_t *primary_verify_label = NULL;
static lv_obj_t *secondary_cards[SECONDARY_SESSIONS] = {};
static lv_obj_t *secondary_dots[SECONDARY_SESSIONS] = {};
static lv_obj_t *secondary_statuses[SECONDARY_SESSIONS] = {};
static lv_obj_t *secondary_titles[SECONDARY_SESSIONS] = {};
```

- [ ] **Step 2: Add small helper functions before `create_status_ui()`**

Add:

```cpp
static String status_label_text(const CodexSession &session)
{
  if (session.status_zh.length()) return session.status_zh;
  if (session.state == "active") return "工作中";
  if (session.state == "complete") return "已完成";
  if (session.state == "blocked") return "已阻塞";
  if (session.state == "notLoaded") return "未加载";
  return "未知";
}

static String next_step_text(const String &state)
{
  if (state == "active") return "下一步：继续处理当前任务";
  if (state == "blocked") return "下一步：等待处理阻塞";
  if (state == "complete") return "下一步：查看结果或收尾";
  if (state == "notLoaded") return "下一步：加载会话后继续";
  return "下一步：等待状态更新";
}

static int state_progress_width(const String &state)
{
  if (state == "complete") return 286;
  if (state == "active") return 186;
  if (state == "notLoaded") return 100;
  if (state == "blocked") return 58;
  return 32;
}
```

- [ ] **Step 3: Rewrite `create_status_ui()`**

Build the screen as a row flex container with three panels:

- companion panel: role image, `Codex`, state, connection summary
- primary panel: status row, title, meta, progress bar, next and verify chips
- secondary panel: two fixed cards

Every panel and card should call `lv_obj_clear_flag(..., LV_OBJ_FLAG_SCROLLABLE)`. Primary and secondary titles should use `LV_LABEL_LONG_DOT`.

- [ ] **Step 4: Rewrite `update_status_ui()` binding**

Use `build_visible_session_order(status, order)` and bind:

- `order[0]` to primary labels and progress.
- `order[1]` and `order[2]` to `secondary_cards[0]` and `secondary_cards[1]`.
- no-session state to `暂无会话`, `下一步：等待状态更新`, and hidden secondary cards.

Always update `assistant_image` with `image_for_status(connection, status)` and keep `log_status_snapshot(connection, status)`.

- [ ] **Step 5: Run focused test and verify GREEN**

Run:

```powershell
python -m unittest tests.test_codex_status_images -v
```

Expected: PASS.

### Task 3: Run Broader Static Regression Tests

**Files:**
- Test: Python unittest suite

- [ ] **Step 1: Run all Python tests**

Run:

```powershell
python -m unittest discover -s tests -v
```

Expected: PASS. If a test fails because it encodes the old layout, update the test only when the new assertion matches the approved Layout A spec.

### Task 4: Firmware Verification

**Files:**
- Test: `verify_codex_status.cmd`

- [ ] **Step 1: Run full board verification**

Run:

```powershell
.\verify_codex_status.cmd
```

Expected: compile succeeds, upload succeeds, serial capture includes `CODEX_STATUS http_` and `CODEX_STATUS ui_update`.

- [ ] **Step 2: If upload or serial access is unavailable**

Run:

```powershell
.\compile_codex_status.cmd
```

Expected: compile succeeds. Report that full device verification could not be completed and include the exact failing step from `verify_codex_status.cmd`.
