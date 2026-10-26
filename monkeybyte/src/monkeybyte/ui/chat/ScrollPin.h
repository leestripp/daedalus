#pragma once

#include <gtkmm/scrolledwindow.h>

namespace monkeybyte {

// GTK's deceleration callback keeps writing scroll positions after a
// programmatic pin. set_kinetic_scrolling(false) cancels that callback in GTK
// 4.22; restore the user's setting immediately afterwards.
inline void cancel_scroller_deceleration(Gtk::ScrolledWindow& scroller) {
    const bool kinetic = scroller.get_kinetic_scrolling();
    if (!kinetic) return;
    scroller.set_kinetic_scrolling(false);
    scroller.set_kinetic_scrolling(true);
}

} // namespace monkeybyte
