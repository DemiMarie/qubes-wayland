//! Geometry routines
//!
//! These functions are responsible for adjusting window positions.
use smithay::utils::{Buffer, Point, Rectangle, Size};

fn adjust_damage_source_single(x: i32, w: i32, min_x: i32) -> Option<(i32, i32)> {
    debug_assert!(
        x >= 0 && w >= 0 && min_x >= 0,
        "bad parameters should be rejected earlier"
    );
    Some(if x < min_x {
        let delta_x = min_x - x;
        debug_assert!(delta_x > 0);
        if delta_x >= w {
            return None; // no damage on visible area
        }
        debug_assert_eq!(w - delta_x + min_x, w + x);
        (min_x, w - delta_x)
    } else {
        (x, w)
    })
}

/// Crop and translate the given damage for the window geometry.  Returns an
/// offset relative to the source buffer, or [`None`] if the damage is
/// entirely outside the visible portion of the window.
pub(crate) fn adjust_damage_source(
    damage: Rectangle<i32, Buffer>,
    geometry: Rectangle<i32, Buffer>,
) -> Option<Rectangle<i32, Buffer>> {
    let (x, w) = adjust_damage_source_single(damage.loc.x, damage.size.w, geometry.loc.x)?;
    let (y, h) = adjust_damage_source_single(damage.loc.y, damage.size.h, geometry.loc.y)?;
    debug_assert_eq!(x + w, damage.loc.x + damage.size.w);
    debug_assert_eq!(y + h, damage.loc.y + damage.size.h);
    Some(Rectangle {
        loc: Point::from((x, y)),
        size: Size::from((w.min(geometry.size.w), h.min(geometry.size.h))),
    })
}

/// Crop and translate the given damage for the window geometry.  Returns an
/// position relative to the destination buffer, or [`None`] if the damage is
/// entirely outside the visible portion of the window.
pub(crate) fn adjust_damage_destination(
    damage: Rectangle<i32, Buffer>,
    geometry: Rectangle<i32, Buffer>,
) -> Option<Rectangle<i32, Buffer>> {
    let (x, w) = adjust_damage_source_single(damage.loc.x, damage.size.w, geometry.loc.x)?;
    let (y, h) = adjust_damage_source_single(damage.loc.y, damage.size.h, geometry.loc.y)?;
    debug_assert_eq!(x + w, damage.loc.x + damage.size.w);
    debug_assert_eq!(y + h, damage.loc.y + damage.size.h);
    Some(Rectangle {
        loc: Point::from((x - geometry.loc.x, y - geometry.loc.y)),
        size: Size::from((w.min(geometry.size.w), h.min(geometry.size.h))),
    })
}
