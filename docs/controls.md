---
title: Controls
nav_order: 6
---

# Controls

The **Controls** menu lets you decide what each button does and, on touchscreen
devices, what each tap and swipe does while you read. You do not need to change
anything to start reading. The defaults work for most people.

To open it, go to **Settings > Controls**. Some entries only appear on devices
that have the matching hardware:

| Entry | Where it appears |
| ----- | ---------------- |
| **Home Button** | Xteink X4 Pro (it has a Home key) |
| **Power Button** | All devices |
| **Front Buttons** | Button devices (Xteink X3 and X4) |
| **Side Buttons** | All devices |
| **Quick Actions** | All devices |
| **Taps & Gestures** | Touchscreen devices (Sticky and X4 Pro) |
| **Tilt Page Turn**, **Tilt Direction** | Devices with a tilt sensor (X3 and Sticky) |
| **Menu navigation** | Button devices |

## Settings Menu Layout

### Home Button

X4 Pro only. See [Home Button (X4 Pro)](#home-button-x4-pro) below.

### Power Button

- **Short-press** action
- **Long-press** action
- **Power + Up Combo** action
- **Quick-return from Footnotes** (appears only after you assign **Footnotes** to a button)

### Front Buttons

- **Remap Buttons**
- **Remap Buttons (Reader)**
- **Orientation Aware** (**No**, **Nav Buttons**, or **All Buttons**)
- **Long-press** behavior (in the reader only): **Off**, **Chapter Skip**, **Change Font Size**, or **Orientation Change**
- **Long-press Back** action (in the reader only)
- **Long-press Menu** action (in the reader only)

> [!NOTE]
> Some of these actions could work everywhere, but the front buttons mean
> different things on different screens. So these actions only apply inside the
> reader.

### Side Buttons

- **Orientation Aware**
- **Up + Down Combo** (X4 and touchscreen devices except EEGO)
- **Left/Up** short-press and long-press actions
- **Right/Down** short-press and long-press actions

### Taps & Gestures

Touchscreen devices only. The two-finger entries need a screen that can
detect more than one finger at a time.

- **Next Page**
- **Previous Page**
- **Pinch to Resize Font** (multi-touch only)
- **Two-finger Screen Rotation** (multi-touch only)
- **Tap to Hide Status Bar**
- **Two-finger Swipe** (multi-touch only)
- **Edge Gestures**

### Menu navigation

Button devices only. **Directional** is the default. Choose **Legacy** to get
the older button behavior back in Settings and the Reader Menu.

### Next Page and Previous Page Gestures

On touchscreen devices, **Next Page** and **Previous Page** are in
**Settings > Controls > Taps & Gestures**. They decide how you turn pages with
your finger, and you can set each one on its own. In left-to-right books, **Next
Page** controls taps and left swipes; **Previous Page** controls taps and right
swipes. Right-to-left EPUBs reverse the normal tap zones and horizontal swipe
directions to follow reading order.

Both settings offer the same options:

| Option | Taps | Swipes |
| ------ | ---- | ------ |
| **Tap & Swipe** (default) | Enabled | Enabled |
| **Tap Only** | Enabled | Disabled |
| **Swipe Only** | Disabled | Enabled |
| **Inverted Tap** | Enabled, with reversed tap zones | Disabled |
| **Disabled** | Disabled | Disabled |

By default, tap the left third of the page to go back and the right two-thirds
to go forward.

> [!NOTE]
> If either setting is **Inverted Tap**, the zones flip: the left
> two-thirds go forward and the right third goes back. If only one direction
> allows taps, a tap anywhere on the page turns in that direction. The top and
> bottom bands of the screen are kept for vertical gestures, so taps there do
> not turn pages. A **Previous Page** setting that does not allow swipes also
> stops a right swipe from the left edge from acting as Back/Home while you
> read.

## Pinch to Resize Font

**Pinch to Resize Font** is on by default on touchscreens that detect two
fingers. While reading an EPUB or TXT book, move two fingers apart to make the
text bigger, or together to make it smaller. Each pinch changes the font by one
size step. You can turn it off in **Settings > Controls > Taps & Gestures**.
It needs **Touch Reader Controls** to be on. It cannot resize XTC books,
because their pages are saved as images.

## Two-finger Screen Rotation

On touchscreen devices with multi-touch support, **Two-finger Screen Rotation**
is enabled by default in **Settings > Controls > Taps & Gestures**. While
reading an EPUB or TXT book, place two fingers on the page and twist them to
rotate the reading orientation one step, the same as changing **Reading
Orientation** by hand. The new orientation is saved, so it survives sleep and
restart. Turn the toggle off if accidental twists rotate the page while you
read; the four-way **Reading Orientation** setting still works.

## Tap to Hide Status Bar

**Tap to Hide Status Bar** is on by default. While reading, tap the status bar
to hide it, and tap the same spot again to bring it back. Hiding it does not
change page layout or page breaks, and it only lasts for the current reading
session. To choose what the top and bottom bars show, see **Status Bars** in
[Reader Features](./reader-features.md#reader-status-bars). The tap works while
**Touch Reader Controls** is on. In XTC books with an XTC status bar turned on,
tap its top or bottom edge to hide and restore it.

## Two-finger Swipe Actions

On touchscreens that detect two fingers, you can give each of the four swipe
directions a job. To set them up, open **Settings > Controls > Taps & Gestures
> Two-finger Swipe** and choose an action for **Swipe Up**, **Swipe Down**,
**Swipe Left**, and **Swipe Right**. To use one, put two fingers on the page
and move them together in that direction. **Not Set** leaves a direction
unused (this is the default for all four).

Each action can be assigned to only one direction. If you give an action to a
new direction, CrossInk clears it from the old one.

Available actions depend on the device and the book:

| Action | Availability |
| --- | --- |
| **Increase Brightness** / **Decrease Brightness** | Devices with a frontlight |
| **Increase Warmth** / **Decrease Warmth** | Devices with an adjustable warm/cool frontlight |
| **Select Chapter** | EPUB and XTC books with a chapter list |
| **Next Chapter** / **Previous Chapter** | EPUB books |
| **Increase Font Size** / **Decrease Font Size** | EPUB and TXT books |
| **Back/Home** | Goes back one screen, or to the Home screen if there is nowhere to go back to |
| **Home/Reader** | From a book, goes to the Home screen. From a screen opened on top of a book (such as the Reader Menu), returns to the book |

Brightness and warmth change while you drag, and a longer swipe gives finer
adjustments. Use a clear, mostly straight motion so both fingers are read as
one gesture. **Select Chapter** opens the chapter list. In XTC books, the
chapter and font-size actions do nothing, because the pages are saved
images.

## Edge Gestures

On touchscreen devices, open **Settings > Controls > Taps & Gestures > Edge
Gestures**. You can give a job to each of **Left Edge Up**, **Left Edge Down**,
**Right Edge Up**, and **Right Edge Down**. To use one, put one finger close to
the left or right edge of the screen and slide up or down along that edge. All
four start as **Not Set**.

The available actions are the same as in
[Two-finger Swipe Actions](#two-finger-swipe-actions): brightness and warmth on
supported hardware, **Select Chapter**, chapter changes, font-size changes, and
**Back/Home** and **Home/Reader**. Unlike two-finger swipes, edge slides need
only one finger, and the same action can be used on more than one edge.
Brightness and warmth change while you drag. Other actions run when you lift
your finger. XTC books cannot be resized or use the EPUB chapter actions.

## Side Button Actions

Open **Settings > Controls > Side Buttons** to give each side button a
**Short-press** action (a quick press) and a **Long-press** action (hold for
about 0.7 seconds). You can set **Left/Up** and **Right/Down** separately.
These actions apply while reading. A long press does not also run the short
action.

By default, **Left/Up** goes to the previous page (long press: previous
chapter) and **Right/Down** goes to the next page (long press: next chapter).

Alongside the shortcut actions listed [below](#power-back-and-menu-button-actions),
you can choose:

- **Previous Page** or **Next Page** to swap page-turn directions or make both
  buttons advance.
- **Previous Chapter** or **Next Chapter** for EPUB chapter navigation.
- **Increase Font Size** or **Decrease Font Size** for EPUB/TXT text.
- **Rotate Page CCW**, **Rotate Page CW**, or **Rotate Page 180°** for EPUB/TXT
  orientation changes. The labels describe the direction the page turns.
- **Ignore** to leave a press unassigned.

XTC books have pages saved as images, so font-size and page-rotation actions do not
apply. **Orientation Aware** is a separate switch that makes the buttons follow
the way you hold the device.

## Power, Back, and Menu Button Actions

These settings let you put a shortcut on a button press you would not
otherwise use.

Defaults:

- Power **Short-press**: **Ignore**
- Power **Long-press**: **Sleep/Wake**
- **Long-press Back** (button devices, in the reader): **Browse Files**
- **Long-press Menu** (button devices, in the reader): **Ignore**
- **Power + Up Combo**: off

Available actions include:

- **Ignore**
- **Sleep** (also available to the combo shortcuts and the Home button). On the
  Power button this appears as **Sleep/Wake**, together with **Sleep** and
  **Wake**, which do only one of the two.
- **Next Page**, **Previous Page**
- **Select Chapter**
- **Toggle Bookmark**
- **Reading Stats** (only when reading stats tracking is on)
- **Mark as Finished**
- **Refresh Screen**
- **Change Font**, **Guide Dots**, **Focus Reading**, **Dark Mode**
- **Auto Page Turn**
- **Tilt Page Turn** (devices with a tilt sensor)
- **Sync Book**, **Nearby Position Sync**
- **Library**, **Browse Files**
- **File Transfer**, **Calibre Wireless**, **Join a Network**, **Create Hotspot**
- **Take Screenshot**
- **Footnotes**
- **Create Clipping**, **Look Up Word**
- **Quick Lock**, **Quick Actions**
- **Back/Home**, **Home/Reader**
- **Toggle Frontlight** (devices with a frontlight)
- **Toggle Touchscreen** (touchscreen devices)

Not every list offers every action. For example, **Long-press Back** and
**Long-press Menu** have no **Next Page**, **Previous Page**, or **Nearby
Position Sync**, and the **Power + Up Combo** has no **Tilt Page Turn**. Actions
for hardware your device does not have are hidden.

Power short- and long-press actions are separate. A short press wakes the
device only when its **Short-press** action is **Sleep/Wake** or **Wake**.
**Sleep** only puts an awake device to sleep; **Wake** does nothing while the
device is awake. Holding Power always wakes a sleeping device. For the full
startup instructions, see [Power On / Off](./user-guide.md#power-on--off).

Assign **Library** to the Power button, **Long-press Back** or **Long-press
Menu**, the combo shortcuts, the Home button, or Quick Actions to open the book
list directly.

## Power + Up Shortcut

The **Power + Up** shortcut runs the action selected in **Settings > Controls >
Power Button > Power + Up Combo**. It is off by default. Press the **Power** and
**Volume Up** buttons together to use it, then release both before using
another shortcut.

It can run most of the same actions as the Power button, including **Quick
Lock**. The **Power + Volume Down** screenshot shortcut is separate and always
works.

## Up + Down Shortcut

On the Xteink X4 and on touchscreen devices other than EEGO, **Settings >
Controls > Side Buttons > Up + Down Combo** runs an action when you press both side buttons
together. It is off by default. On a touchscreen device, if you have turned the
reader touchscreen off, this combo does something else instead: it opens a menu
so you can always turn the touchscreen back on.

On **EEGO**, Up + Down is reserved for the device's hardware reset. It is not
offered as a shortcut or Quick Actions trigger, on the device or in web settings.
Previously saved Up + Down assignments do not run. If you disable the
touchscreen, restarting the EEGO re-enables it automatically. This reboot
recovery behavior applies only to EEGO.

## Home Button (X4 Pro)

The X4 Pro has a Home key. Open **Settings > Controls > Home Button** to set:

- **In Reader**: **Enabled** or **Disabled**. When disabled, the Home key is
  locked while you read, which helps avoid accidental presses.
- **Tap** (default **Back/Home**)
- **Double Tap** (default **Toggle Frontlight**)
- **Long-press** (default **Reader Menu**)

Each can also run any of the shortcut actions above, plus **Reader Menu**.

## Quick Actions Triggers

**Quick Actions** is a small menu of five actions you pick yourself. Open
it from **Settings > Controls > Quick Actions**, choose one **Shortcut** to open
the menu, and choose the five actions. **Quick Lock** and **Quick Actions**
themselves cannot go in the menu.

A shortcut can only belong to one thing. Choosing a trigger for Quick Actions
replaces whatever that button press did before. The available triggers are
short- or long-press Power, **Power + Up Combo**, **Up + Down Combo**, each
side-button short or long press, and on X4 Pro, **Tap Home**, **Long-Press
Home**, or **Double Tap Home**. Button devices can also use **Long-press Back**
and **Long-press Menu**. EEGO does not offer **Up + Down Combo** as a trigger.

## Quick Lock

**Quick Lock** temporarily disables normal button and touchscreen input while
leaving the current screen visible. A lock badge appears on the display while
the device is locked. It is useful when carrying the reader or setting it down
while reading.

You can assign Quick Lock to any shortcut, such as **Power + Up**, a Power
button press, a side button, or a long press of **Back** or **Menu**. Use the
shortcut to lock. While locked, only that same shortcut unlocks; all other
buttons (including **Power + Volume Down** screenshots) and the touchscreen
stay locked.

Quick Lock still follows the regular **Time to Sleep** setting. If the timeout
expires, the device sleeps and restores the lock when it wakes. Reading timers
pause while Quick Lock is active.

## Footnote Shortcut

When a shortcut is mapped to **Footnotes**, it opens the page-reference selector
(or a list fallback) while reading. If the current page has only one footnote,
CrossInk opens that referenced page directly.

The **Quick-return from Footnotes** setting controls whether the Power button
acts like Back after opening a footnote page. It appears under **Settings >
Controls > Power Button** only after assigning **Footnotes** to Power short/long
press or Back/Menu long press. See [Footnote Navigation](./user-guide.md#footnote-navigation)
for selecting references on the page.
