package com.gtavsource.android;
import android.content.Context;
import android.widget.ImageButton;
import android.widget.LinearLayout;
import android.widget.Switch;
import java.util.function.Consumer;
import java.util.function.IntConsumer;
final class MenuStyle {
    static LinearLayout detailRow(LinearLayout parent, String label, String value) { return null; }
    static ImageButton icon(Context context, int icon, String description, Runnable action) { return null; }
    static void slider(LinearLayout parent, String label, int value, int min, int max, String suffix, IntConsumer changed) {}
    static Switch toggle(LinearLayout parent, String label, String help, boolean value, Consumer<Boolean> changed) { return null; }
}
