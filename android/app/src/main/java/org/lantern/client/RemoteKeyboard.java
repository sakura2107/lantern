package org.lantern.client;

import android.content.Context;
import android.text.Editable;
import android.text.SpannableStringBuilder;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.View;
import android.view.inputmethod.*;

/** Local composition only. Committed text travels to the host; host contents are never downloaded. */
final class RemoteKeyboard extends View {
    interface Sink { void text(String value); void key(int vk, int count); }
    final Sink sink;
    boolean password, accepting;
    final Editable composition = new SpannableStringBuilder();
    RemoteKeyboard(Context c, Sink target) {
        super(c); sink = target; setFocusable(true); setFocusableInTouchMode(true);
        setImportantForAutofill(View.IMPORTANT_FOR_AUTOFILL_NO_EXCLUDE_DESCENDANTS);
    }
    void open(boolean secret) {
        password = secret; accepting = true; composition.clear(); requestFocus();
        InputMethodManager manager = (InputMethodManager)getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
        manager.restartInput(this);
        post(() -> { if (accepting) manager.showSoftInput(this, InputMethodManager.SHOW_IMPLICIT); });
    }
    void close() {
        accepting = false; composition.clear();
        ((InputMethodManager)getContext().getSystemService(Context.INPUT_METHOD_SERVICE))
            .hideSoftInputFromWindow(getWindowToken(), 0);
        clearFocus();
    }
    @Override public boolean onCheckIsTextEditor() { return accepting; }
    @Override public InputConnection onCreateInputConnection(EditorInfo info) {
        if (!accepting) return null;
        info.inputType = InputType.TYPE_CLASS_TEXT | (password ? InputType.TYPE_TEXT_VARIATION_PASSWORD : InputType.TYPE_TEXT_FLAG_MULTI_LINE);
        info.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_NO_FULLSCREEN |
                          EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING | EditorInfo.IME_ACTION_NONE;
        info.initialSelStart = info.initialSelEnd = 0;
        return new BaseInputConnection(this, true) {
            @Override public Editable getEditable() { return composition; }
            @Override public boolean setComposingText(CharSequence value, int cursor) {
                if (!accepting) return false; return super.setComposingText(value, cursor);
            }
            @Override public boolean finishComposingText() {
                if (!accepting) return false;
                if (composition.length() > 0) { sink.text(composition.toString()); composition.clear(); }
                return super.finishComposingText();
            }
            @Override public boolean commitText(CharSequence value, int cursor) {
                if (!accepting) return false;
                // Composition remains local until this callback, including Chinese candidate selection.
                sink.text(value.toString()); composition.clear(); return true;
            }
            @Override public boolean deleteSurroundingText(int before, int after) {
                if (!accepting || before < 0 || after < 0 || before > 32 || after > 32) return false;
                if (composition.length() > 0) return super.deleteSurroundingText(before, after);
                if (before > 0) sink.key(8, before);
                if (after > 0) sink.key(46, after);
                return true;
            }
            @Override public boolean deleteSurroundingTextInCodePoints(int before, int after) {
                return deleteSurroundingText(before, after);
            }
            @Override public boolean sendKeyEvent(KeyEvent e) {
                if (!accepting) return false;
                if (e.getAction() != KeyEvent.ACTION_DOWN) return true;
                int vk = key(e.getKeyCode());
                if (vk != 0) { sink.key(vk, 1); return true; }
                int ch = e.getUnicodeChar();
                if (ch >= 32) { sink.text(new String(Character.toChars(ch))); return true; }
                return false;
            }
            @Override public boolean performEditorAction(int action) { if (!accepting) return false; sink.key(13, 1); return true; }
        };
    }
    static int key(int code) {
        switch (code) {
        case KeyEvent.KEYCODE_DEL: return 8;
        case KeyEvent.KEYCODE_FORWARD_DEL: return 46;
        case KeyEvent.KEYCODE_ENTER: return 13;
        case KeyEvent.KEYCODE_TAB: return 9;
        case KeyEvent.KEYCODE_ESCAPE: return 27;
        case KeyEvent.KEYCODE_DPAD_LEFT: return 37;
        case KeyEvent.KEYCODE_DPAD_UP: return 38;
        case KeyEvent.KEYCODE_DPAD_RIGHT: return 39;
        case KeyEvent.KEYCODE_DPAD_DOWN: return 40;
        case KeyEvent.KEYCODE_MOVE_HOME: return 36;
        case KeyEvent.KEYCODE_MOVE_END: return 35;
        default: return 0;
        }
    }
}
