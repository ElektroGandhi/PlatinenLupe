// Check model: dummies for both boards inside the case, collisions as intersections.
// One call per check: openscad -o x.stl -D 'check="..."' check.scad
//   "Current top level object is empty" = no overlap (wanted for collision_*).
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

pcb_z = back_t + cable_h;      // PCB underside
function y0(pcb) = wall + usb_margin;

module board(b) {
    mx = wall + b[0]; pcb = b[1]; row = b[2]; n = b[3]; mod = b[4];
    // PCB
    translate([mx - pcb[0]/2, y0(pcb), pcb_z]) cube([pcb[0], pcb[1], pcb_t]);
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
module boards() { for (b = boards) board(b); }

module plug_path() {   // USB-C plug from outside up to the socket
    for (b = boards) translate([wall + b[0] - plug[0]/2, -40, pcb_z + pcb_t + usbc[2]/2 - plug[1]/2]) cube([plug[0], 40 + y0(b[1]) - 1 - 0.01, plug[1]]);
}
module lid_mounted() { translate([0, 0, body_t]) lid(); }
module hold_down_contact() {   // thin layer directly above each PCB
    for (b = boards) translate([wall + b[0] - b[1][0]/2, y0(b[1]), pcb_z + pcb_t]) cube([b[1][0], b[1][1], 0.15]);
}

if (check == "collision_body")     intersection() { body(); boards(); }
if (check == "collision_lid")      intersection() { lid_mounted(); boards(); }
if (check == "collision_plug")     intersection() { union() { body(); lid_mounted(); } plug_path(); }
if (check == "collision_lid_body") intersection() { body(); lid_mounted(); }
if (check == "collision_quiver")   intersection() { body(); quiver(); }
if (check == "collision_pen")      intersection() { quiver(); pen_dummy(); }
if (check == "contact_hold_downs") intersection() { lid_mounted(); hold_down_contact(); }   // must NOT be empty
if (check == "boards")             boards();
