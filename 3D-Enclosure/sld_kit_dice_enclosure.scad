// Copyright (C) 2026-2026 Author Name <a.name@is-bg.net>
// Copyright (C) 2026-2026 Information Services JSC <https://www.is-bg.net>
// SPDX-License-Identifier: EUPL-1.2
//
// SLD-KIT-DICE enclosure, v2.
// Board data from SLD-KIT-DICE_Rev_A.kicad_pcb / .kicad_sch:
//   - Edge.Cuts: 40 x 40 mm, corner radius 6 mm
//   - 7 x 5 mm LEDs on F.Cu (top): corners, centre and two mid-sides,
//     at +/-15 mm from board centre. They are the only top-side parts.
//   - Everything else is on B.Cu (bottom): MCU, CR2032 holder, speaker,
//     shock sensor, transistor, resistors, header.
// Architecture: the LEDs point up, so the lid (top half) holds the board and
// the LEDs show through holes in the top face. The base is an empty cup.
//
// ESTIMATES TO VERIFY: led_h, bot_comp_h, battery / speaker / sensor envelopes.
$fn=120;
part = "print";   // "print" | "base" | "lid" | "assembly"

// ---------- Board ----------
board_width       = 40;
board_radius      = 6;      // board corner radius (not used in geometry; kept for reference)
board_thickness   = 1.6;
led_h             = 8.6;    // 5 mm LED, PCB top to lens top (typical; check datasheet)
led_hole          = 5.3;    // hole in top face for each LED body
bot_comp_h        = 6;     // tallest bottom-side part below PCB (speaker is 5.5 mm)
clr               = 1.5;    // board clearance per side

// LED centres relative to board centre (from KiCad: LED1..LED7)
led_xy = [[-15,-15], [-15,0], [-15,15], [15,15], [15,0], [15,-15], [0,0]];

// ---------- Shell ----------
H         = 48;     // dice edge
wall      = 2.5;
r         = 6;      // dice corner radius
t         = 1.0;    // lid skirt thickness
seam_clr  = 0.2;
sh        = 4;      // skirt height below seam
tab_w     = 8;
slot_w    = 1;
bump_h    = 0.8;
bump_hz   = 1.6;
zs        = 26;     // seam height (base below, lid above)

// ---------- Derived ----------
board_top = H - led_h;
board_bot = board_top - board_thickness;     // 37.8 with defaults

assert(board_width + 2*clr <= H - 2*wall, "board does not fit the shell");
assert(board_top <= H - wall, "LED bodies do not fit under the top face");
assert(board_bot - bot_comp_h >= zs + 1, "bottom components reach the seam");

// ---------- Helpers ----------
module rounded_cube(s, rr) {
    hull()
        for (x = [rr, s-rr], y = [rr, s-rr], z = [rr, s-rr])
            translate([x, y, z]) sphere(r = rr, $fn = 48);
}

module inset_shape(d) {
    translate([d, d, d]) rounded_cube(H - 2*d, r - d);
}

module cavity() {
    translate([wall, wall, wall]) rounded_cube(H - 2*wall,4);
}

// Children placed on side k (0..3); side 0 is the -Y face.
module at_side(k) {
    translate([H/2, H/2, 0]) rotate([0, 0, 90*k]) translate([-H/2, -H/2, 0])
        children();
}

// ---------- Snap-fit (defined on -Y side, rotated to all four sides) ----------
module front_slots() {
    for (x = [H/2 - tab_w/2 - slot_w, H/2 + tab_w/2])
        translate([x, -1, zs - sh - 1]) cube([slot_w, t + 1.3, sh]);
}

module front_bump() {
    zb = zs - sh;
    translate([H/2 - tab_w/2 + 0.3, 0, 0]) rotate([90, 0, 90])
        linear_extrude(tab_w - 0.6)
            polygon([[t - 0.1, zb], [t + bump_h, zb + bump_h],
                     [t + bump_h, zb + bump_hz], [t - 0.1, zb + bump_hz]]);
}

module front_window() {
    zb = zs - sh;
    translate([H/2 - tab_w/2 - 0.2, -1, zb - 0.3])
        cube([tab_w + 0.4, wall + 1.5, bump_hz + 0.6]);
}

// ---------- Board support (lid side, corner blocks under PCB corners) ----------
// Blocks sit at the inner corners, clear of the battery holder and speaker.
module board_supports() {
    z0 = board_bot - 1.5;
    bs = 5;
    for (x0 = [wall, H - wall - bs], y0 = [wall, H - wall - bs])
        translate([x0, y0, z0]) cube([bs, bs, 1.5 + 0.01]);
}

// ---------- LED windows in the top face ----------
module led_holes() {
    for (p = led_xy)
        translate([H/2 + p[0], H/2 + p[1], H - wall - 1])
            cylinder(d = led_hole, h = wall + 2, $fn = 32);
}

// ---------- Base: bottom cup, seam at zs ----------
module base_region() {
    union() {
        intersection() {
            rounded_cube(H, r);
            translate([-1, -1, -1]) cube([H + 2, H + 2, zs - sh + 1]);
        }
        intersection() {
            inset_shape(t + seam_clr);
            translate([-1, -1, zs - sh]) cube([H + 2, H + 2, sh]);
        }
    }
}

module base_part() {
    difference() {
        base_region();
        cavity();
        for (k = [0:3]) at_side(k) front_window();
    }
}

// ---------- Lid: top half with LED windows, board supports and snap skirt ----------
module lid_region() {
    union() {
        intersection() {
            rounded_cube(H, r);
            translate([-1, -1, zs]) cube([H + 2, H + 2, H - zs + 2]);
        }
        intersection() {
            difference() {
                rounded_cube(H, r);
                inset_shape(t);
            }
            translate([-1, -1, zs - sh]) cube([H + 2, H + 2, sh]);
        }
    }
}

module lid_part() {
    union() {
        difference() {
            lid_region();
            cavity();
            for (k = [0:3]) at_side(k) front_slots();
            led_holes();
        }
        board_supports();
        for (k = [0:3]) at_side(k) front_bump();
    }
}

// ---------- Output ----------
if (part == "base") {
    base_part();
} else if (part == "lid") {
    lid_part();
} else if (part == "print") {
    // Base: seam up on the bed. Lid: flipped so the top face sits on the bed.
    base_part();
    translate([H + 10, H, H]) rotate([180, 0, 0]) lid_part();
} else {
    base_part();
    lid_part();
}
