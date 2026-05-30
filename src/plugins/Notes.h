#pragma once

#include "../config.h"

#if FEATURE_PLUGINS

#include <Arduino.h>
#include <SDCardManager.h>
#include <Utf8.h>
#include "PluginHelpers.h"
#include "PluginInterface.h"
#include "PluginRenderer.h"

namespace sumi {

// =============================================================================
// Notes — Landscape-mode distraction-free writing app (BYOK-style)
//
// Designed for BLE keyboard input. When connected, user gets a clean wide
// writing surface with minimal chrome. Physical buttons handle navigation
// when no keyboard is present.
// Contains shortcuts for common markdown syntax: bold, italics, headings
// =============================================================================
// Shortcut table
// Key 	    Code 	Action
// Ctrl+A 	0x01 	Home (start of line)
// Ctrl+B 	0x02 	Bold — wrap word in **...** (toggle)
// Ctrl+E 	0x05 	End (end of line)
// Ctrl+H 	0x08 	Heading — add/increment # at line start (up to H6)
// Ctrl+I 	0x09 	Italic — wrap word in *...* (toggle)
// Ctrl+K 	0x0B 	Delete to end of line
// Ctrl+S 	0x13 	Save
// Ctrl+U 	0x15 	Delete to start of line
// ← → ↑ ↓ 	ANSI ESC 	Cursor movement (3-byte escape sequence)
// =============================================================================

class NotesApp;
extern NotesApp* g_notesInstance;

class NotesApp : public PluginInterface {
public:
  const char* name() const override { return "Notes"; }
  PluginRunMode runMode() const override { return PluginRunMode::WithUpdate; }
  bool wantsLandscape() const override { return true; }

  static constexpr int MAX_NOTES = 20;
  static constexpr int MAX_NAME_LEN = 32;
  static constexpr int BUFFER_SIZE = 4096;
  static constexpr int AUTO_SAVE_MS = 3000;

  enum Screen {
    SCREEN_FILE_LIST,
    SCREEN_EDITOR,
    SCREEN_NEW_NOTE,
  };

  explicit NotesApp(PluginRenderer& renderer);
  ~NotesApp();
  void init(int screenW, int screenH) override;
  bool handleInput(PluginButton btn) override;
  bool handleChar(char c) override;
  void draw() override;
  bool update() override;
  void cleanup() override;

  PluginRenderer& d_;

private:
  // File management
  char notes_[MAX_NOTES][MAX_NAME_LEN];
  int noteCount_ = 0;
  char currentFile_[64] = {0};

  // Text buffer
  char buf_[BUFFER_SIZE];
  int bufLen_ = 0;
  int cursorPos_ = 0;
  bool modified_ = false;
  unsigned long lastKeystroke_ = 0;

  // New note naming
  char newName_[MAX_NAME_LEN];
  int newNameLen_ = 0;

  // UI state
  Screen screen_ = SCREEN_FILE_LIST;
  int listCursor_ = 0;
  int listScroll_ = 0;
  int viewScrollLine_ = 0;
  // ANSI escape sequence state for arrow keys
  // BLE keyboards send arrows as ESC [ A/B/C/D (3 bytes)
  enum EscState { ESC_NONE = 0, ESC_GOT_ESC = 1, ESC_GOT_BRACKET = 2 };
  EscState escState_ = ESC_NONE;

  // Layout (computed in init)
  int W_ = 800, H_ = 480;
  int statusBarH_ = 28;
  int marginX_ = 16;
  int marginY_ = 8;
  int lineH_ = 22;
  int charW_ = 10;
  int maxLineWidth_ = 768;  // pixel width available for text
  int linesVisible_ = 18;
  int editTop_ = 36;
  int editBottom_ = 450;

  // File list layout
  int itemH_ = 40;
  int itemsPerPage_ = 10;

  // Methods
  void reset();
  void computeLayout();
  void scanNotes();
  int countWords() const;
  int countLines() const;

  // Editor
  void openNote(int idx);
  void createNote(const char* noteName);
  void saveNote();
  void insertChar(char c);
  void deleteCharBack();
  void deleteCharForward();
  void moveCursorUp();
  void moveCursorDown();
  void moveCursorHome();      // Ctrl+A / Home
  void moveCursorEnd();       // Ctrl+E / End
  void deleteLine();          // Ctrl+K
  void toggleBold();          // Ctrl+B, wrap with **
  void toggleItalic();        // Ctrl+I, wrap with *
  void addHeading();          // Ctrl+H, prepend line with #

  void ensureCursorVisible();
  int cursorToLine() const;
  int cursorToCol() const;
  int lineColToPos(int line, int col) const;

 // Pixel-accurate line wrapping (mutable: computed cache, not logical state)
  struct WrapLine { int start; int len; };  // byte-offset, byte-length
  static constexpr int MAX_WRAP_LINES = 256;
  mutable WrapLine wrapLines_[MAX_WRAP_LINES];
  mutable int wrapLineCount_ = 0;
  void buildWrapLines() const;
  int cursorWrapLine() const;
  int cursorWrapCol() const;  // pixel offset within wrap line

  // Drawing
  void drawFileList();
  void drawEditor();
  void drawNewNote();
  void drawStatusBar(const char* left, const char* right);
};


// =============================================================================
// IMPLEMENTATION
// =============================================================================

NotesApp* g_notesInstance = nullptr;

NotesApp::NotesApp(PluginRenderer& renderer) : d_(renderer) {
  g_notesInstance = this;
  reset();
}

NotesApp::~NotesApp() {
  g_notesInstance = nullptr;
}

void NotesApp::reset() {
  noteCount_ = 0;
  bufLen_ = 0;
  buf_[0] = '\0';
  cursorPos_ = 0;
  modified_ = false;
  lastKeystroke_ = 0;
  currentFile_[0] = '\0';
  newName_[0] = '\0';
  newNameLen_ = 0;
  screen_ = SCREEN_FILE_LIST;
  listCursor_ = 0;
  listScroll_ = 0;
  viewScrollLine_ = 0;
  escState_ = ESC_NONE;
  wrapLineCount_ = 0;
}

void NotesApp::init(int screenW, int screenH) {
  W_ = screenW;
  H_ = screenH;
  computeLayout();
  scanNotes();
  screen_ = SCREEN_FILE_LIST;
  Serial.printf("[NOTES] Init %dx%d, maxLineWidth=%d, linesVisible=%d\n",
                W_, H_, maxLineWidth_, linesVisible_);
}

void NotesApp::computeLayout() {
  charW_ = d_.getTextWidth("M");
  if (charW_ < 6) charW_ = 10;
  lineH_ = d_.getLineHeight() + 4;
  if (lineH_ < 14) lineH_ = 22;

  statusBarH_ = lineH_ + 8;
  editTop_ = statusBarH_ + marginY_;
  editBottom_ = H_ - marginY_ - 4;
  maxLineWidth_ = W_ - 2 * marginX_ - 8;  // -8 for scroll indicator
  linesVisible_ = (editBottom_ - editTop_) / lineH_;

  itemH_ = lineH_ + 14;
  itemsPerPage_ = (H_ - statusBarH_ - 40) / itemH_;

  Serial.printf("[NOTES] Layout: charW=%d lineH=%d maxLineWidth=%d linesVis=%d\n",
                charW_, lineH_, maxLineWidth_, linesVisible_);
}

void NotesApp::cleanup() {
  if (modified_) saveNote();
}

void NotesApp::scanNotes() {
  noteCount_ = 0;

  if (!SdMan.exists("/notes")) {
    SdMan.mkdir("/notes");
  }

  FsFile dir = SdMan.open("/notes");
  if (!dir) return;

  while (FsFile entry = dir.openNextFile()) {
    if (noteCount_ >= MAX_NOTES - 1) { entry.close(); break; }

    char fname[64];
    entry.getName(fname, sizeof(fname));
    if (fname[0] == '.') { entry.close(); continue; }

    int len = strlen(fname);
    if (len > 4 && strcasecmp(fname + len - 4, ".txt") == 0) {
      utf8SafeCopy(notes_[noteCount_], fname, MAX_NAME_LEN);
      noteCount_++;
    }
    entry.close();
  }
  dir.close();
}

int NotesApp::countWords() const {
  int words = 0;
  bool inWord = false;
  for (int i = 0; i < bufLen_; i++) {
    bool ws = (buf_[i] == ' ' || buf_[i] == '\n' || buf_[i] == '\t');
    if (!ws && !inWord) words++;
    inWord = !ws;
  }
  return words;
}

int NotesApp::countLines() const {
  // Use the wrap line table for accuracy
  buildWrapLines();
  return wrapLineCount_ > 0 ? wrapLineCount_ : 1;
}

// =============================================================================
// Pixel-accurate line wrapping
// =============================================================================

// Helper: length in bytes of the UTF-8 codepoint starting at buf[pos].
static int notesCodepointLen(const char* buf, int pos, int bufLen) {
  if (pos >= bufLen) return 1;
  const unsigned char c = static_cast<unsigned char>(buf[pos]);
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

void NotesApp::buildWrapLines() const {
  wrapLineCount_ = 0;
  if (bufLen_ == 0) {
    wrapLines_[0] = {0, 0};
    wrapLineCount_ = 1;
    return;
  }

  int lineStart = 0;   // byte offset where current visual line begins
  int pixelW = 0;       // accumulated pixel width of current visual line
  int wordStart = -1;   // byte offset of current word (for word-wrap)
  int wordPixelW = 0;   // pixel width of current word

  int i = 0;
  while (i <= bufLen_) {
    if (i < bufLen_ && buf_[i] == '\n') {
      if (wrapLineCount_ < MAX_WRAP_LINES) {
        wrapLines_[wrapLineCount_++] = {lineStart, i - lineStart};
      }
      i++;
      lineStart = i;
      pixelW = 0;
      wordStart = -1;
      continue;
    }

    if (i >= bufLen_) {
      if (wrapLineCount_ < MAX_WRAP_LINES) {
        wrapLines_[wrapLineCount_++] = {lineStart, i - lineStart};
      }
      break;
    }

    int cpLen = notesCodepointLen(buf_, i, bufLen_);
    if (i + cpLen > bufLen_) cpLen = bufLen_ - i;

    // Copy codepoint to temp buffer instead of mutating buf_
    char tmp[5] = {};
    for (int k = 0; k < cpLen && k < 4; k++) tmp[k] = buf_[i + k];
    int cpW = d_.getTextWidth(tmp);
    if (cpW < 1) cpW = charW_;

    bool isSpace = (buf_[i] == ' ' || buf_[i] == '\t');

    if (isSpace) {
      wordStart = -1;
      wordPixelW = 0;
    } else if (wordStart < 0) {
      wordStart = i;
      wordPixelW = 0;
    }

    if (pixelW + cpW > maxLineWidth_ && i > lineStart) {
      if (wordStart > lineStart && wordStart != i) {
        if (wrapLineCount_ < MAX_WRAP_LINES) {
          wrapLines_[wrapLineCount_++] = {lineStart, wordStart - lineStart};
        }
        lineStart = wordStart;
        // Recompute pixelW from lineStart to i
        pixelW = 0;
        for (int j = lineStart; j < i; ) {
          int jl = notesCodepointLen(buf_, j, bufLen_);
          if (j + jl > bufLen_) jl = bufLen_ - j;
          char tmp2[5] = {};
          for (int k = 0; k < jl && k < 4; k++) tmp2[k] = buf_[j + k];
          pixelW += d_.getTextWidth(tmp2);
          j += jl;
        }
        pixelW += cpW;
        wordStart = lineStart;
        wordPixelW = pixelW;
      } else {
        if (wrapLineCount_ < MAX_WRAP_LINES) {
          wrapLines_[wrapLineCount_++] = {lineStart, i - lineStart};
        }
        lineStart = i;
        pixelW = cpW;
        wordStart = isSpace ? -1 : i;
        wordPixelW = isSpace ? 0 : cpW;
      }
    } else {
      pixelW += cpW;
      if (!isSpace && wordStart >= 0) {
        wordPixelW += cpW;
      }
    }

    i += cpLen;
  }

  if (wrapLineCount_ == 0) {
    wrapLines_[0] = {0, 0};
    wrapLineCount_ = 1;
  }
}

int NotesApp::cursorWrapLine() const {
  for (int l = 0; l < wrapLineCount_; l++) {
    int end = wrapLines_[l].start + wrapLines_[l].len;
    if (cursorPos_ <= end) return l;
  }
  return wrapLineCount_ - 1;
}

int NotesApp::cursorWrapCol() const {
  int l = cursorWrapLine();
  int start = wrapLines_[l].start;
  int px = 0;
  for (int i = start; i < cursorPos_ && i < bufLen_; ) {
    int cpLen = notesCodepointLen(buf_, i, bufLen_);
    if (i + cpLen > bufLen_) cpLen = bufLen_ - i;
    // Copy codepoint to temp buffer instead of mutating buf_
    char tmp[5] = {};
    for (int k = 0; k < cpLen && k < 4; k++) tmp[k] = buf_[i + k];
    px += d_.getTextWidth(tmp);
    i += cpLen;
  }
  return px;
}

// =============================================================================
// Text buffer operations
// =============================================================================

void NotesApp::insertChar(char c) {
  if (bufLen_ >= BUFFER_SIZE - 2) return;
  memmove(buf_ + cursorPos_ + 1, buf_ + cursorPos_, bufLen_ - cursorPos_);
  buf_[cursorPos_] = c;
  cursorPos_++;
  bufLen_++;
  buf_[bufLen_] = '\0';
  modified_ = true;
  lastKeystroke_ = millis();
  ensureCursorVisible();
}

void NotesApp::deleteCharBack() {
  if (cursorPos_ <= 0) return;
  int del = 1;
  while (del < cursorPos_
         && (static_cast<unsigned char>(buf_[cursorPos_ - del]) & 0xC0) == 0x80) {
    del++;
  }
  cursorPos_ -= del;
  memmove(buf_ + cursorPos_, buf_ + cursorPos_ + del, bufLen_ - cursorPos_ - del);
  bufLen_ -= del;
  buf_[bufLen_] = '\0';
  modified_ = true;
  lastKeystroke_ = millis();
  ensureCursorVisible();
}

void NotesApp::deleteCharForward() {
  if (cursorPos_ >= bufLen_) return;
  int del = 1;
  const unsigned char lead = static_cast<unsigned char>(buf_[cursorPos_]);
  if (lead >= 0xC0) {
    if ((lead & 0xE0) == 0xC0) del = 2;
    else if ((lead & 0xF0) == 0xE0) del = 3;
    else if ((lead & 0xF8) == 0xF0) del = 4;
  }
  if (cursorPos_ + del > bufLen_) del = bufLen_ - cursorPos_;
  memmove(buf_ + cursorPos_, buf_ + cursorPos_ + del, bufLen_ - cursorPos_ - del);
  bufLen_ -= del;
  buf_[bufLen_] = '\0';
  modified_ = true;
  lastKeystroke_ = millis();
}

void NotesApp::moveCursorHome() {
  // Move to start of current visual line
  int l = cursorWrapLine();
  cursorPos_ = wrapLines_[l].start;
  ensureCursorVisible();
}

void NotesApp::moveCursorEnd() {
  // Move to end of current visual line (before newline if any)
  int l = cursorWrapLine();
  int end = wrapLines_[l].start + wrapLines_[l].len;
  // If the line ends with a newline, stop before it
  if (end > 0 && end <= bufLen_ && buf_[end - 1] == '\n') end--;
  cursorPos_ = end;
  ensureCursorVisible();
}

void NotesApp::deleteLine() {
  // Delete from cursor to end of visual line
  int l = cursorWrapLine();
  int lineEnd = wrapLines_[l].start + wrapLines_[l].len;
  // Include the newline if present
  if (lineEnd < bufLen_ && buf_[lineEnd - 1] == '\n') {
    // already included in len
  }
  int delStart = wrapLines_[l].start;
  int delCount = lineEnd - delStart;
  if (delCount <= 0) return;
  memmove(buf_ + delStart, buf_ + lineEnd, bufLen_ - lineEnd);
  bufLen_ -= delCount;
  buf_[bufLen_] = '\0';
  cursorPos_ = delStart;
  if (cursorPos_ > bufLen_) cursorPos_ = bufLen_;
  modified_ = true;
  lastKeystroke_ = millis();
  ensureCursorVisible();
}

void NotesApp::toggleBold() {
  // Find word boundaries around cursor
  int wordStart = cursorPos_;
  int wordEnd = cursorPos_;

  // Backtrack to start of word
  while (wordStart > 0 && buf_[wordStart - 1] != ' '
         && buf_[wordStart - 1] != '\n' && buf_[wordStart - 1] != '*') {
    wordStart--;
  }
  // Forward to end of word
  while (wordEnd < bufLen_ && buf_[wordEnd] != ' '
         && buf_[wordEnd] != '\n' && buf_[wordEnd] != '*') {
    wordEnd++;
  }

  // Check if word is already wrapped in **
  bool alreadyBold = false;
  if (wordEnd + 2 <= bufLen_ && wordStart >= 2) {
    if (buf_[wordStart - 2] == '*' && buf_[wordStart - 1] == '*'
        && buf_[wordEnd] == '*' && buf_[wordEnd + 1] == '*') {
      alreadyBold = true;
    }
  }

  if (alreadyBold) {
    // Remove trailing **
    memmove(buf_ + wordEnd, buf_ + wordEnd + 2, bufLen_ - wordEnd - 1);
    bufLen_ -= 2;
    // Remove leading ** (positions shifted by -2)
    int adjStart = wordStart - 2;
    memmove(buf_ + adjStart, buf_ + adjStart + 2, bufLen_ - adjStart - 1);
    bufLen_ -= 2;
    buf_[bufLen_] = '\0';
    cursorPos_ = adjStart + (wordEnd - wordStart);
    if (cursorPos_ > bufLen_) cursorPos_ = bufLen_;
  } else {
    if (bufLen_ + 4 < BUFFER_SIZE) {
      // Insert trailing ** first (so wordStart doesn't shift)
      memmove(buf_ + wordEnd + 2, buf_ + wordEnd, bufLen_ - wordEnd + 1);
      buf_[wordEnd] = '*';
      buf_[wordEnd + 1] = '*';
      bufLen_ += 2;
      // Insert leading **
      memmove(buf_ + wordStart + 2, buf_ + wordStart, bufLen_ - wordStart + 1);
      buf_[wordStart] = '*';
      buf_[wordStart + 1] = '*';
      bufLen_ += 2;
      buf_[bufLen_] = '\0';
      cursorPos_ = wordEnd + 4;
      if (cursorPos_ > bufLen_) cursorPos_ = bufLen_;
    }
  }

  modified_ = true;
  lastKeystroke_ = millis();
  ensureCursorVisible();
}

void NotesApp::toggleItalic() {
  int wordStart = cursorPos_;
  int wordEnd = cursorPos_;

  while (wordStart > 0 && buf_[wordStart - 1] != ' '
         && buf_[wordStart - 1] != '\n' && buf_[wordStart - 1] != '*') {
    wordStart--;
  }
  while (wordEnd < bufLen_ && buf_[wordEnd] != ' '
         && buf_[wordEnd] != '\n' && buf_[wordEnd] != '*') {
    wordEnd++;
  }

  // Check if already wrapped in single * (not **)
  bool alreadyItalic = false;
  if (wordEnd + 1 <= bufLen_ && wordStart >= 1) {
    if (buf_[wordStart - 1] == '*' && buf_[wordEnd] == '*') {
      bool leadingDouble = (wordStart >= 2 && buf_[wordStart - 2] == '*');
      bool trailingDouble = (wordEnd + 1 < bufLen_ && buf_[wordEnd + 1] == '*');
      if (!leadingDouble && !trailingDouble) {
        alreadyItalic = true;
      }
    }
  }

  if (alreadyItalic) {
    // Remove trailing *
    memmove(buf_ + wordEnd, buf_ + wordEnd + 1, bufLen_ - wordEnd);
    bufLen_ -= 1;
    // Remove leading *
    int adjStart = wordStart - 1;
    memmove(buf_ + adjStart, buf_ + adjStart + 1, bufLen_ - adjStart);
    bufLen_ -= 1;
    buf_[bufLen_] = '\0';
    cursorPos_ = adjStart + (wordEnd - wordStart);
    if (cursorPos_ > bufLen_) cursorPos_ = bufLen_;
  } else {
    if (bufLen_ + 2 < BUFFER_SIZE) {
      // Insert trailing * first
      memmove(buf_ + wordEnd + 1, buf_ + wordEnd, bufLen_ - wordEnd + 1);
      buf_[wordEnd] = '*';
      bufLen_ += 1;
      // Insert leading *
      memmove(buf_ + wordStart + 1, buf_ + wordStart, bufLen_ - wordStart + 1);
      buf_[wordStart] = '*';
      bufLen_ += 1;
      buf_[bufLen_] = '\0';
      cursorPos_ = wordEnd + 2;
      if (cursorPos_ > bufLen_) cursorPos_ = bufLen_;
    }
  }

  modified_ = true;
  lastKeystroke_ = millis();
  ensureCursorVisible();
}

void NotesApp::addHeading() {
  buildWrapLines();
  int l = cursorWrapLine();
  int lineStart = wrapLines_[l].start;

  // Count existing # at line start
  int hashCount = 0;
  int pos = lineStart;
  while (pos < bufLen_ && buf_[pos] == '#') {
    hashCount++;
    pos++;
  }
  // Skip space after hashes if any
  if (pos < bufLen_ && buf_[pos] == ' ') pos++;

  if (hashCount > 0 && hashCount < 6) {
    // Increment heading level: add one more #
    if (bufLen_ + 1 < BUFFER_SIZE) {
      memmove(buf_ + lineStart + hashCount + 1,
              buf_ + lineStart + hashCount,
              bufLen_ - lineStart - hashCount + 1);
      buf_[lineStart + hashCount] = '#';
      bufLen_ += 1;
      buf_[bufLen_] = '\0';
      cursorPos_ = pos + 1;
      if (cursorPos_ > bufLen_) cursorPos_ = bufLen_;
    }
  } else if (hashCount == 0) {
    // No heading yet — insert "# " at line start
    if (bufLen_ + 2 < BUFFER_SIZE) {
      memmove(buf_ + lineStart + 2, buf_ + lineStart, bufLen_ - lineStart + 1);
      buf_[lineStart] = '#';
      buf_[lineStart + 1] = ' ';
      bufLen_ += 2;
      buf_[bufLen_] = '\0';
      cursorPos_ += 2;
      if (cursorPos_ > bufLen_) cursorPos_ = bufLen_;
    }
  }
  // hashCount == 6: max heading, do nothing

  modified_ = true;
  lastKeystroke_ = millis();
  ensureCursorVisible();
}


void NotesApp::moveCursorUp() {
  int l = cursorWrapLine();
  if (l <= 0) return;
  int targetPx = cursorWrapCol();
  // Move to previous wrap line, find closest position by pixel
  int prevLine = l - 1;
  int start = wrapLines_[prevLine].start;
  int end = start + wrapLines_[prevLine].len;
  // Walk forward measuring pixels until we reach targetPx or line end
  int px = 0;
  int bestPos = start;
  int bestDist = abs(targetPx);
  for (int i = start; i < end && i < bufLen_; ) {
    if (buf_[i] == '\n') break;
    int cpLen = notesCodepointLen(buf_, i, bufLen_);
    if (i + cpLen > bufLen_) cpLen = bufLen_ - i;
    char saved = buf_[i + cpLen];
    buf_[i + cpLen] = '\0';
    px += d_.getTextWidth(&buf_[i]);
    buf_[i + cpLen] = saved;
    int dist = abs(px - targetPx);
    if (dist < bestDist) { bestDist = dist; bestPos = i + cpLen; }
    i += cpLen;
  }
  cursorPos_ = bestPos;
  ensureCursorVisible();
}

void NotesApp::moveCursorDown() {
  int l = cursorWrapLine();
  if (l >= wrapLineCount_ - 1) return;
  int targetPx = cursorWrapCol();
  int nextLine = l + 1;
  int start = wrapLines_[nextLine].start;
  int end = start + wrapLines_[nextLine].len;
  int px = 0;
  int bestPos = start;
  int bestDist = abs(targetPx);
  for (int i = start; i < end && i < bufLen_; ) {
    if (buf_[i] == '\n') break;
    int cpLen = notesCodepointLen(buf_, i, bufLen_);
    if (i + cpLen > bufLen_) cpLen = bufLen_ - i;
    char saved = buf_[i + cpLen];
    buf_[i + cpLen] = '\0';
    px += d_.getTextWidth(&buf_[i]);
    buf_[i + cpLen] = saved;
    int dist = abs(px - targetPx);
    if (dist < bestDist) { bestDist = dist; bestPos = i + cpLen; }
    i += cpLen;
  }
  cursorPos_ = bestPos;
  ensureCursorVisible();
}

int NotesApp::cursorToLine() const {
  return cursorWrapLine();
}

int NotesApp::cursorToCol() const {
  return cursorWrapCol() / charW_;  // approximate column for compat
}

int NotesApp::lineColToPos(int line, int col) const {
  if (line >= wrapLineCount_) line = wrapLineCount_ - 1;
  if (line < 0) line = 0;
  int start = wrapLines_[line].start;
  int targetPx = col * charW_;
  int px = 0;
  int i = start;
  int end = start + wrapLines_[line].len;
  while (i < end && i < bufLen_) {
    if (buf_[i] == '\n') break;
    int cpLen = notesCodepointLen(buf_, i, bufLen_);
    if (i + cpLen > bufLen_) cpLen = bufLen_ - i;
    char tmp[5] = {};
    for (int k = 0; k < cpLen && k < 4; k++) tmp[k] = buf_[i + k];
    px += d_.getTextWidth(tmp);
    if (px > targetPx) break;
    i += cpLen;
  }
  return i;
}


void NotesApp::ensureCursorVisible() {
  buildWrapLines();
  int curLine = cursorWrapLine();
  if (curLine < viewScrollLine_) {
    viewScrollLine_ = curLine;
  } else if (curLine >= viewScrollLine_ + linesVisible_) {
    viewScrollLine_ = curLine - linesVisible_ + 1;
  }
}

// =============================================================================
// File operations
// =============================================================================

void NotesApp::openNote(int idx) {
  if (idx >= noteCount_) return;

  snprintf(currentFile_, sizeof(currentFile_), "/notes/%s", notes_[idx]);

  FsFile f = SdMan.open(currentFile_);
  if (!f) { bufLen_ = 0; buf_[0] = '\0'; return; }

  bufLen_ = f.read((uint8_t*)buf_, BUFFER_SIZE - 1);
  if (bufLen_ < 0) bufLen_ = 0;
  buf_[bufLen_] = '\0';
  f.close();

  cursorPos_ = bufLen_;
  modified_ = false;
  viewScrollLine_ = 0;
  ensureCursorVisible();

  Serial.printf("[NOTES] Opened %s (%d bytes, %d words)\n",
                currentFile_, bufLen_, countWords());
}

void NotesApp::createNote(const char* noteName) {
  snprintf(currentFile_, sizeof(currentFile_), "/notes/%s.txt", noteName);

  FsFile f;
  if (SdMan.atomicOpenWrite("NOTES", currentFile_, f)) {
    if (!SdMan.atomicCommit(f, currentFile_)) {
      SdMan.atomicAbort(f, currentFile_);
    }
  }

  bufLen_ = 0;
  buf_[0] = '\0';
  cursorPos_ = 0;
  modified_ = false;
  viewScrollLine_ = 0;

  if (noteCount_ < MAX_NOTES - 1) {
    snprintf(notes_[noteCount_], MAX_NAME_LEN, "%s.txt", noteName);
    noteCount_++;
  }
}

void NotesApp::saveNote() {
  if (currentFile_[0] == '\0') return;

  FsFile f;
  if (!SdMan.atomicOpenWrite("NOTES", currentFile_, f)) return;
  f.write((uint8_t*)buf_, bufLen_);
  if (!SdMan.atomicCommit(f, currentFile_)) {
    SdMan.atomicAbort(f, currentFile_);
    return;
  }
  modified_ = false;
  Serial.printf("[NOTES] Saved %s (%d bytes)\n", currentFile_, bufLen_);
}

// =============================================================================
// Input handling — now with BLE keyboard shortcuts
// =============================================================================

bool NotesApp::handleChar(char c) {
  // ---- ANSI escape sequence handling (arrow keys from BLE keyboard) ----
  // BLE HID keyboards send arrow keys as: ESC (0x1B) [ (0x5B) A/B/C/D
  if (escState_ == ESC_GOT_BRACKET) {
    escState_ = ESC_NONE;
    if (screen_ == SCREEN_EDITOR) {
      switch (c) {
        case 'A':  // Up
          moveCursorUp();
          needsFullRedraw = true;
          return true;
        case 'B':  // Down
          moveCursorDown();
          needsFullRedraw = true;
          return true;
        case 'C':  // Right
          if (cursorPos_ < bufLen_) {
            const unsigned char lead = static_cast<unsigned char>(buf_[cursorPos_]);
            int step = 1;
            if ((lead & 0xE0) == 0xC0) step = 2;
            else if ((lead & 0xF0) == 0xE0) step = 3;
            else if ((lead & 0xF8) == 0xF0) step = 4;
            cursorPos_ += step;
            if (cursorPos_ > bufLen_) cursorPos_ = bufLen_;
            ensureCursorVisible();
          }
          needsFullRedraw = true;
          return true;
        case 'D':  // Left
          if (cursorPos_ > 0) {
            cursorPos_--;
            while (cursorPos_ > 0
                   && (static_cast<unsigned char>(buf_[cursorPos_]) & 0xC0) == 0x80) {
              cursorPos_--;
            }
            ensureCursorVisible();
          }
          needsFullRedraw = true;
          return true;
        default:
          break;  // unknown CSI sequence, discard
      }
    }
    return false;
  }

  if (escState_ == ESC_GOT_ESC) {
    if (c == '[') {
      escState_ = ESC_GOT_BRACKET;
      return true;  // waiting for final byte
    }
    // Not a bracket — it's a standalone ESC press
    escState_ = ESC_NONE;
    // Fall through to handle ESC below
  }

  // Start of escape sequence?
  if (c == 27) {
    escState_ = ESC_GOT_ESC;
    return true;
  }

  if (screen_ == SCREEN_EDITOR) {
    // Ctrl+key shortcuts (BLE keyboards send ctrl chars as 0x01..0x1A)
    if (c == 1) {  // Ctrl+A → Home
      moveCursorHome();
      needsFullRedraw = true;
      return true;
    }
    if (c == 2) {  // Ctrl+B → Bold **...**
      toggleBold();
      needsFullRedraw = true;
      return true;
    }
    if (c == 5) {  // Ctrl+E → End
      moveCursorEnd();
      needsFullRedraw = true;
      return true;
    }
    if (c == 8) {  // Ctrl+H → Add/increment heading #
      addHeading();
      needsFullRedraw = true;
      return true;
    }
    if (c == 9) {  // Ctrl+I → Italic *...*
      toggleItalic();
      needsFullRedraw = true;
      return true;
    }
    if (c == 11) { // Ctrl+K → Delete to end of line
      deleteLine();
      needsFullRedraw = true;
      return true;
    }
    if (c == 19) { // Ctrl+S → Save
      saveNote();
      needsFullRedraw = true;
      return true;
    }
    if (c == 21) { // Ctrl+U → Delete to start of line
      {
        int l = cursorWrapLine();
        int lineStart = wrapLines_[l].start;
        int delCount = cursorPos_ - lineStart;
        if (delCount > 0) {
          memmove(buf_ + lineStart, buf_ + cursorPos_, bufLen_ - cursorPos_);
          bufLen_ -= delCount;
          buf_[bufLen_] = '\0';
          cursorPos_ = lineStart;
          modified_ = true;
          lastKeystroke_ = millis();
          ensureCursorVisible();
        }
      }
      needsFullRedraw = true;
      return true;
    }

    // Standard characters
    if (c == '\b') {
      deleteCharBack();
    } else if (c == 127) {
      deleteCharForward();
    } else if (c >= 32 || c == '\n' || c == '\t') {
      if (c == '\t') {
        insertChar(' ');
        insertChar(' ');
      } else {
        insertChar(c);
      }
    }
    needsFullRedraw = true;
    return true;
  }

  if (screen_ == SCREEN_NEW_NOTE) {
    if (c == '\b') {
      if (newNameLen_ > 0) {
        newNameLen_--;
        newName_[newNameLen_] = '\0';
      }
    } else if (c == '\n') {
      if (newNameLen_ > 0) {
        createNote(newName_);
        screen_ = SCREEN_EDITOR;
      }
    } else if (c == 27) {
      screen_ = SCREEN_FILE_LIST;
    } else if (c >= 32 && c < 127 && newNameLen_ < MAX_NAME_LEN - 5) {
      newName_[newNameLen_++] = c;
      newName_[newNameLen_] = '\0';
    }
    needsFullRedraw = true;
    return true;
  }

  if (screen_ == SCREEN_FILE_LIST && c == '\n') {
    if (listCursor_ == noteCount_) {
      newName_[0] = '\0';
      newNameLen_ = 0;
      screen_ = SCREEN_NEW_NOTE;
    } else {
      openNote(listCursor_);
      screen_ = SCREEN_EDITOR;
    }
    needsFullRedraw = true;
    return true;
  }

  return false;
}

bool NotesApp::handleInput(PluginButton btn) {
  if (screen_ == SCREEN_FILE_LIST) {
    switch (btn) {
      case PluginButton::Up:
        if (listCursor_ > 0) {
          listCursor_--;
          if (listCursor_ < listScroll_) listScroll_ = listCursor_;
        }
        return true;
      case PluginButton::Down:
        if (listCursor_ < noteCount_) {
          listCursor_++;
          if (listCursor_ >= listScroll_ + itemsPerPage_) listScroll_++;
        }
        return true;
      case PluginButton::Center:
        if (listCursor_ == noteCount_) {
          newName_[0] = '\0';
          newNameLen_ = 0;
          screen_ = SCREEN_NEW_NOTE;
        } else {
          openNote(listCursor_);
          screen_ = SCREEN_EDITOR;
        }
        return true;
      case PluginButton::Back:
        return false;
      default:
        return false;
    }
  }

  if (screen_ == SCREEN_EDITOR) {
    switch (btn) {
      case PluginButton::Up:    moveCursorUp(); return true;
      case PluginButton::Down:  moveCursorDown(); return true;
      case PluginButton::Left: {
        if (cursorPos_ > 0) {
          cursorPos_--;
          while (cursorPos_ > 0
                 && (static_cast<unsigned char>(buf_[cursorPos_]) & 0xC0) == 0x80) {
            cursorPos_--;
          }
          ensureCursorVisible();
        }
        return true;
      }
      case PluginButton::Right: {
        if (cursorPos_ < bufLen_) {
          const unsigned char lead = static_cast<unsigned char>(buf_[cursorPos_]);
          int step = 1;
          if ((lead & 0xE0) == 0xC0) step = 2;
          else if ((lead & 0xF0) == 0xE0) step = 3;
          else if ((lead & 0xF8) == 0xF0) step = 4;
          cursorPos_ += step;
          if (cursorPos_ > bufLen_) cursorPos_ = bufLen_;
          ensureCursorVisible();
        }
        return true;
      }
      case PluginButton::Back:
        if (modified_) saveNote();
        screen_ = SCREEN_FILE_LIST;
        scanNotes();
        return true;
      case PluginButton::Center:
        insertChar('\n');
        needsFullRedraw = true;
        return true;
      default:
        return false;
    }
  }

  if (screen_ == SCREEN_NEW_NOTE) {
    switch (btn) {
      case PluginButton::Up:
        if (newNameLen_ > 0) {
          char& c = newName_[newNameLen_ - 1];
          c = (c >= 'a' && c < 'z') ? c + 1 : (c == 'z') ? '0' :
              (c >= '0' && c < '9') ? c + 1 : 'a';
        }
        return true;
      case PluginButton::Down:
        if (newNameLen_ > 0) {
          char& c = newName_[newNameLen_ - 1];
          c = (c > 'a' && c <= 'z') ? c - 1 : (c == 'a') ? '9' :
              (c > '0' && c <= '9') ? c - 1 : 'z';
        }
        return true;
      case PluginButton::Right:
        if (newNameLen_ < MAX_NAME_LEN - 5) {
          newName_[newNameLen_++] = 'a';
          newName_[newNameLen_] = '\0';
        }
        return true;
      case PluginButton::Left:
        if (newNameLen_ > 0) {
          newNameLen_--;
          newName_[newNameLen_] = '\0';
        }
        return true;
      case PluginButton::Center:
        if (newNameLen_ > 0) {
          createNote(newName_);
          screen_ = SCREEN_EDITOR;
        }
        return true;
      case PluginButton::Back:
        screen_ = SCREEN_FILE_LIST;
        return true;
      default:
        return false;
    }
  }

  return false;
}

bool NotesApp::update() {
  if (screen_ == SCREEN_EDITOR && modified_ && lastKeystroke_ > 0) {
    if (millis() - lastKeystroke_ > AUTO_SAVE_MS) {
      saveNote();
      lastKeystroke_ = 0;
      needsFullRedraw = true;
      return true;
    }
  }
  return false;
}

// =============================================================================
// Drawing
// =============================================================================

void NotesApp::drawStatusBar(const char* left, const char* right) {
  d_.drawLine(0, statusBarH_ - 1, W_, statusBarH_ - 1, GxEPD_BLACK);

  d_.setTextColor(GxEPD_BLACK);
  d_.setCursor(marginX_, statusBarH_ - 8);
  d_.print(left);

  if (right && right[0]) {
    int rw = d_.getTextWidth(right);
    d_.setCursor(W_ - marginX_ - rw, statusBarH_ - 8);
    d_.print(right);
  }
}

void NotesApp::draw() {
  d_.fillScreen(GxEPD_WHITE);

  switch (screen_) {
    case SCREEN_FILE_LIST: drawFileList(); break;
    case SCREEN_EDITOR:    drawEditor(); break;
    case SCREEN_NEW_NOTE:  drawNewNote(); break;
  }
}

void NotesApp::drawFileList() {
  drawStatusBar("Notes", "");

  int y = statusBarH_ + 8;
  int displayCount = noteCount_ + 1;

  for (int i = listScroll_; i < displayCount && i < listScroll_ + itemsPerPage_; i++) {
    bool sel = (i == listCursor_);
    int x = marginX_;
    int w = W_ - 2 * marginX_;

    if (sel) {
      d_.drawRect(x, y + 1, w, itemH_ - 2, GxEPD_BLACK);
      d_.drawRect(x + 1, y + 2, w - 2, itemH_ - 4, GxEPD_BLACK);
    }

    d_.setCursor(x + 12, y + itemH_ / 2 + 4);
    d_.setTextColor(GxEPD_BLACK);

    if (i == noteCount_) {
      d_.print("+ New Note");
    } else {
      char display[MAX_NAME_LEN];
      utf8SafeCopy(display, notes_[i], sizeof(display));
      int len = strlen(display);
      if (len > 4 && strcasecmp(display + len - 4, ".txt") == 0) {
        display[len - 4] = '\0';
      }
      d_.print(display);
    }

    if (!sel && i < noteCount_) {
      d_.drawLine(x + 10, y + itemH_ - 1, x + w - 10, y + itemH_ - 1, GxEPD_BLACK);
    }

    y += itemH_;
  }

  d_.setCursor(marginX_, H_ - 20);
  d_.print("OK: Open    Back: Exit");
}

void NotesApp::drawEditor() {
  // Build wrap lines for accurate rendering
  buildWrapLines();

  // Status bar
  const char* fname = strrchr(currentFile_, '/');
  fname = fname ? fname + 1 : currentFile_;
  char titleBuf[48];
  utf8SafeCopy(titleBuf, fname, sizeof(titleBuf));
  int tlen = strlen(titleBuf);
  if (tlen > 4 && strcasecmp(titleBuf + tlen - 4, ".txt") == 0) {
    titleBuf[tlen - 4] = '\0';
  }
  if (modified_) strncat(titleBuf, " *", sizeof(titleBuf) - strlen(titleBuf) - 1);

  char rightStatus[32];
  snprintf(rightStatus, sizeof(rightStatus), "%d words", countWords());

  drawStatusBar(titleBuf, rightStatus);

  // Draw wrapped text lines
  int y = editTop_;
  int cursorScreenX = -1, cursorScreenY = -1;

  for (int l = viewScrollLine_; l < wrapLineCount_ && l < viewScrollLine_ + linesVisible_; l++) {
    int screenY = y + (l - viewScrollLine_) * lineH_;
    if (screenY >= editBottom_) break;

    int lineStart = wrapLines_[l].start;
    int lineLen = wrapLines_[l].len;
    int screenX = marginX_;
    int px = 0;  // accumulated pixel offset

    for (int i = lineStart; i < lineStart + lineLen && i < bufLen_; ) {
      // Check if cursor is at this position
      if (i == cursorPos_) {
        cursorScreenX = screenX + px;
        cursorScreenY = screenY;
      }

      const unsigned char lead = static_cast<unsigned char>(buf_[i]);

      // Skip newlines in rendering (they cause line breaks, not glyphs)
      if (lead == '\n') {
        if (i == cursorPos_) {
          cursorScreenX = screenX + px;
          cursorScreenY = screenY;
        }
        i++;
        continue;
      }

      // Measure and render one codepoint
      int cpLen = notesCodepointLen(buf_, i, bufLen_);
      if (i + cpLen > bufLen_) cpLen = bufLen_ - i;

      char saved = buf_[i + cpLen];
      buf_[i + cpLen] = '\0';
      int cpW = d_.getTextWidth(&buf_[i]);
      if (cpW < 1) cpW = charW_;

      // Render at pixel-accurate position
      d_.setCursor(screenX + px, screenY + lineH_ - 4);
      d_.print(&buf_[i]);

      buf_[i + cpLen] = saved;
      px += cpW;
      i += cpLen;
    }

    // Cursor at end of line
    if (cursorPos_ >= lineStart && cursorPos_ <= lineStart + lineLen) {
      if (cursorScreenX < 0) {
        cursorScreenX = screenX + px;
        cursorScreenY = screenY;
      }
    }
  }

  // Cursor: thin vertical bar
  if (cursorScreenX >= 0 && cursorScreenY >= 0) {
    d_.fillRect(cursorScreenX, cursorScreenY + 2, 2, lineH_ - 4, GxEPD_BLACK);
  }

  // Scroll indicator
  int totalLines = wrapLineCount_;
  if (totalLines > linesVisible_) {
    int trackH = editBottom_ - editTop_ - 20;
    int thumbH = (trackH * linesVisible_) / totalLines;
    if (thumbH < 10) thumbH = 10;
    int maxScroll = totalLines - linesVisible_;
    if (maxScroll < 1) maxScroll = 1;
    int thumbY = editTop_ + 10 + (trackH - thumbH) * viewScrollLine_ / maxScroll;
    d_.fillRect(W_ - 4, thumbY, 3, thumbH, GxEPD_BLACK);
  }
}

void NotesApp::drawNewNote() {
  drawStatusBar("New Note", "");

  int centerY = H_ / 2;

  d_.setCursor(marginX_, centerY - 40);
  d_.print("Note name:");

  int fieldX = marginX_;
  int fieldY = centerY - 10;
  int fieldW = W_ / 2;
  int fieldH = lineH_ + 8;
  d_.drawRect(fieldX, fieldY, fieldW, fieldH, GxEPD_BLACK);

  d_.setCursor(fieldX + 6, fieldY + fieldH - 6);
  if (newNameLen_ > 0) {
    d_.print(newName_);
  }
  int curX = fieldX + 6 + d_.getTextWidth(newName_);
  d_.fillRect(curX, fieldY + 4, 2, fieldH - 8, GxEPD_BLACK);

  d_.setCursor(marginX_, centerY + 40);
  d_.print("Type name or use Up/Down/L/R    OK: Create    Back: Cancel");
}

}  // namespace sumi

#endif  // FEATURE_PLUGINS
