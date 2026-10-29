// Check model: dummies for both boards inside the case, collisions as intersections.
// One call per check: openscad -o x.stl -D 'check="..."' check.scad
//   "Current top level object is empty" = no overlap (wanted for collision_*).
//   contact_*, blocked_*, retention_* must NOT be empty: retention_* moves a part 0.3-0.5 mm against its
//   snap direction -- only a barb, nub or latch that really catches gives an overlap.
// All assumptions about the boards are collected here; anything not confirmed is marked.

include <platinenlupe_case.scad>
part = "none";
check = "collision_body";

// ---- Board assumptions ---------------------------------------------------------------------
// [width, length, row spacing, pins per row, module w x l x h (at the antenna end)]
boards = [
    // NodeMCU v3 LoLin: 58x31, rows 28 mm (two sources); ESP-12: 16x24x3.8 (typical value, not confirmed)
    [x_node, nodemcu, 28.0, 15, [16, 24, 3.8]],
    // ESP32 DevKit V1 30 pins: 51.8x28.2 (one source); rows 25.4 mm (only confirmed for DevKitC)
    [x_esp,  esp32,   25.4, 15, [18, 25.5, 3.9]],
];
usbc   = [8.94, 7.35, 3.26];   // USB-C socket w x d x h (standard Type-C socket, typical value), protrudes 1 mm
button = [6.0, 3.5, 3.5];      // buttons next to the USB socket (EN/BOOT or RST/FLASH), typical value
dupont = [2.6, 2.6, 14.0];     // Dupont socket, pushed on up to the plastic of the pin header
plug   = [12.4, 6.5];          // USB-C plug including strain relief, common size
metal  = [8.4, 2.6];           // bare metal shell of a USB-C plug (USB-C spec 8.25 x 2.4 + tolerance)
micro  = [6.85, 1.8];          // bare shell of a micro-USB plug: the NodeMCU v3 LoLin has micro-USB, not USB-C
                               // (seen on the real board, 2026-10-29) — the smallest plug that must stay out

pcb_z = back_t + cable_h;      // PCB underside
function y0(pcb) = wall + usb_margin;

module pcb(b) { translate([wall + b[0] - b[1][0]/2, y0(b[1]), pcb_z]) cube([b[1][0], b[1][1], pcb_t]); }
module parts_on(b) {
    mx = wall + b[0]; pcb = b[1]; row = b[2]; n = b[3]; mod = b[4];
    // pin headers: plastic under the PCB, pins 6 mm below -- centred along the length (assumption)
    l_row = n * 2.54; ys = y0(pcb) + (pcb[1] - l_row)/2;
    for (s = [-1, 1]) {
        translate([mx + s*row/2 - 1.25, ys, pcb_z - 2.5]) cube([2.5, l_row, 2.5]);
        // Dupont sockets on the outermost pins (worst case)
        for (y = [ys + 1.27, ys + l_row - 1.27]) translate([mx + s*row/2 - dupont[0]/2, y - dupont[1]/2, pcb_z - 2.5 - dupont[2]]) cube(dupont);
    }
    // radio module on top at the antenna end
    translate([mx - mod[0]/2, y0(pcb) + pcb[1] - mod[1], pcb_z + pcb_t]) cube([mod[0], mod[1], mod[2]]);
    // USB-C socket at the USB end, protruding 1 mm
    translate([mx - usbc[0]/2, y0(pcb) - 1, pcb_z + pcb_t]) cube(usbc);
    // buttons left and right of the socket
    for (s = [-1, 1]) translate([mx + s*(usbc[0]/2 + 1.5 + button[0]/2) - button[0]/2, y0(pcb) + 1, pcb_z + pcb_t]) cube(button);
}
module board(b) { pcb(b); parts_on(b); }
module boards() { for (b = boards) board(b); }
module pcbs() { for (b = boards) pcb(b); }
module board_parts() { for (b = boards) parts_on(b); }

// plug from outside up to the socket front; size = plug (with strain relief) or metal (bare shell)
module plug_at(b, size) {
    translate([wall + b[0] - size[0]/2, -40, pcb_z + pcb_t + usbc[2]/2 - size[1]/2]) cube([size[0], 40 + y0(b[1]) - 1 - 0.01, size[1]]);
}
module plug_path() { for (b = boards) plug_at(b, plug); }
module lid_mounted(h = true) { translate([0, 0, body_t]) lid(h); }
module hold_downs_mounted() { translate([0, 0, body_t]) hold_downs(); }
module attached() { fin_mounted(); quiver(); saturn_mounted(); hooks_mounted(); }

// ---- boards and lid
if (check == "collision_body")      intersection() { body(); boards(); }
if (check == "retention_boards")    intersection() { body(); translate([0, 0, 0.3]) pcbs(); }      // latches catch the PCB
if (check == "collision_lid")       { intersection() { lid_mounted(false); boards(); } intersection() { hold_downs_mounted(); board_parts(); } }
if (check == "contact_hold_downs")  intersection() { hold_downs_mounted(); pcbs(); }               // must NOT be empty
if (check == "collision_lid_body")  intersection() { body(); lid_mounted(); }
// ---- parts that click into the walls
if (check == "collision_parts_body")   intersection() { body(); attached(); }
if (check == "collision_parts_boards") intersection() { boards(); attached(); }
if (check == "retention_fin")       intersection() { body(); translate([0.5, 0, 0]) fin_mounted(); }
if (check == "retention_quiver")    intersection() { body(); translate([-0.5, 0, 0]) quiver(); }
if (check == "retention_saturn")    intersection() { body(); translate([0, 0.5, 0]) saturn_mounted(); }
if (check == "retention_hooks")     intersection() { body(); translate([0, 0, -0.5]) hooks_mounted(); }
// the quiver must stay quiver_clear away from the wall everywhere except its web (the funnel jammed at 0.3 mm)
if (check == "clearance_quiver")    intersection() { body(); translate([quiver_clear - 0.01, 0, 0]) quiver_outer(); }
if (check == "collision_pen")       intersection() { quiver(); pen_dummy(); }
// ---- inlays in the lid (lid coordinates)
if (check == "collision_inlays")    intersection() { lid(); inlays(); }
if (check == "retention_inlays")    intersection() { lid(); translate([0, 0, 0.4]) inlays(); }
// ---- USB selector: RUN = NodeMCU free, FLASH = ESP32 free, never both
if (check == "collision_slider_run")   intersection() { body(); slider(0); }
if (check == "collision_slider_flash") intersection() { body(); slider(1); }
if (check == "retention_slider")    intersection() { body(); translate([0, 0, 0.4]) slider(0); }
if (check == "collision_plug_run")  intersection() { union() { body(); lid_mounted(); slider(0); } plug_at(boards[0], plug); }
if (check == "collision_plug_flash") intersection() { union() { body(); lid_mounted(); slider(1); } plug_at(boards[1], plug); }
if (check == "blocked_esp_run")     intersection() { slider(0); plug_at(boards[1], metal); }      // must NOT be empty
if (check == "blocked_node_flash")  intersection() { slider(1); plug_at(boards[0], micro); }
if (check == "blocked_node_mid")    intersection() { slider(0.5); plug_at(boards[0], micro); }
if (check == "blocked_esp_mid")     intersection() { slider(0.5); plug_at(boards[1], metal); }
if (check == "boards")              boards();
