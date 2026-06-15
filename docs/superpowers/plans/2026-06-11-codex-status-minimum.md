# Codex Status Minimum Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a minimum Codex status display loop for the ESP32-S3 Touch-LCD-3.49.

**Architecture:** A local Python bridge serves fixed JSON at `/status`. A new Arduino example based on the existing LVGL V9 screen project connects to Wi-Fi, polls the bridge, parses JSON, and updates compact labels for the 172 x 640 portrait display.

**Tech Stack:** Python standard library, unittest, Arduino ESP32 Wi-Fi/HTTPClient, fixed-field JSON string extraction, LVGL V9.

---

### Task 1: Local Bridge

**Files:**
- Create: `tools/codex_status_bridge.py`
- Create: `tests/test_codex_status_bridge.py`

- [ ] Write failing unittest coverage for the JSON payload and `/status` HTTP response.
- [ ] Run `python -m unittest tests.test_codex_status_bridge -v` and confirm it fails because the bridge module is missing.
- [ ] Implement `build_status_payload()` and `StatusServer`.
- [ ] Run `python -m unittest tests.test_codex_status_bridge -v` and confirm it passes.

### Task 2: ESP32 Arduino Example

**Files:**
- Create: `Examples/Arduino/12_Codex_Status/`
- Modify copied LVGL port code to expose `lvgl_port_lock()` and `lvgl_port_unlock()`.
- Replace the sketch with Wi-Fi, HTTP polling, fixed-field JSON parsing, and LVGL label updates.

- [ ] Copy the existing `Examples/Arduino/10_LVGL_V9_Test` as the hardware baseline.
- [ ] Remove the LVGL demo startup and create a compact Codex status UI.
- [ ] Add user-editable Wi-Fi and bridge URL constants near the top of the sketch.
- [ ] Poll `/status` on a fixed interval and update the UI from parsed JSON.

### Task 3: Verification

**Files:**
- Read: changed files

- [ ] Run the Python bridge tests.
- [ ] Run static searches for expected Arduino includes, URL constants, and LVGL lock calls.
- [ ] Report that Arduino firmware compilation was not run if no Arduino CLI/ESP32 board toolchain is available.
