// SPDX-License-Identifier: MIT
// Copyright (c) 2026 XHYN-PH
package com.gtavsource.android;

import android.app.AlertDialog;
import android.widget.LinearLayout;
import java.util.Arrays;

/** Upscaler rows on Graphics > Display. The native layer reads them from [upscaler] in android.ini. */
final class XhynUpscalerMenu {
    private static final String SECTION = "upscaler";
    private static final String RESTART = " Restart the game to apply.";

    private XhynUpscalerMenu() {}

    static void add(GameMenuActivity a, LinearLayout c) {
        final RuntimeConfig config = a.config;
        MenuStyle.toggle(c, "Upscaling (SGSR 1)",
                "Sharpens the game's image to fill the screen. For more fps, lower Resolution in the game's own"
                        + " graphics menu. Needs HDR10 off." + RESTART,
                "sgsr1".equals(config.get(SECTION, "mode", "off")), value -> {
                    setUpscaling(a, value);
                    refresh(a, c);
                });
        choose(a, c, "Upscaler input", "preset",
                new String[] {"game", "quality", "balanced", "performance", "ultra_performance"},
                new String[] {"Match game resolution (recommended)", "Quality (screen / 1.3)", "Balanced (screen / 1.5)",
                        "Performance (screen / 1.7)", "Ultra performance (screen / 2)"},
                "game");
        MenuStyle.slider(c, "Upscaler sharpening", sharpeningPercent(config), 0, 100, "%",
                value -> a.commit(SECTION, "edge_sharpness", Float.toString(1f + value / 100f)));
        MenuStyle.toggle(c, "Frame generation (experimental)",
                "Shows an extra generated frame between game frames. Adds input lag, can cause artefacts around the HUD"
                        + " and fast camera turns, and works best from 40 fps or more. Turns on upscaling." + RESTART,
                config.getBoolean(SECTION, "frame_gen", false), value -> {
                    a.commit(SECTION, "frame_gen", Boolean.toString(value));
                    if (value) setUpscaling(a, true);
                    refresh(a, c);
                });
        MenuStyle.toggle(c, "Generated frame pacing",
                "Spaces generated and game frames evenly. Off lowers input lag a little but looks less smooth." + RESTART,
                config.getBoolean(SECTION, "frame_gen_pacing", true),
                value -> a.commit(SECTION, "frame_gen_pacing", Boolean.toString(value)));
    }

    private static void setUpscaling(GameMenuActivity a, boolean on) {
        a.commit(SECTION, "mode", on ? "sgsr1" : "off");
        if (on) {
            a.commit("vulkan", "bcn_decode_mode", "on");  // the game only loads the layer with this on
        } else {
            a.commit(SECTION, "frame_gen", "false");
        }
    }

    private static int sharpeningPercent(RuntimeConfig config) {
        try {
            final float sharpness = Float.parseFloat(config.get(SECTION, "edge_sharpness", "2.0"));
            return Math.max(0, Math.min(100, Math.round((sharpness - 1f) * 100f)));
        } catch (NumberFormatException e) {
            return 100;
        }
    }

    // The toggle that called this is still handling its event, so the page is rebuilt afterwards.
    private static void refresh(GameMenuActivity a, LinearLayout c) {
        c.post(() -> a.showPage(a.page));
    }

    private static void choose(GameMenuActivity a, LinearLayout c, String label, String key, String[] values, String[] names,
                               String fallback) {
        final int current = Math.max(0, Arrays.asList(values).indexOf(a.config.get(SECTION, key, fallback)));
        final LinearLayout row = MenuStyle.detailRow(c, label, names[current]);
        row.addView(MenuStyle.icon(a, 0x0108009d, "Choose " + label, () -> {
            AlertDialog.Builder builder = new MenuDialogBuilder(a);
            builder.setTitle(label);
            builder.setSingleChoiceItems(names, current, (dialog, which) -> {
                if (a.commit(SECTION, key, values[which])) {
                    dialog.dismiss();
                    a.showPage(a.page);
                }
            });
            builder.setNegativeButton("Cancel", null);
            builder.show();
        }));
    }
}
