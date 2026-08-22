# Input Methods

## Control Method Guide

Set the control method with:

```ini
input_method = WIN32
```

Valid values:

```text
WIN32, KMBOX_NET, KMBOX_A, MAKCU
```

Hardware methods are explicit. If the selected device is unavailable, the app does not switch to another method for you.

## When WIN32 Does Not Move In Game

`WIN32` sends standard Windows mouse events through the normal Win32 input path. This is useful for a quick desktop test, but some games ignore or block this kind of synthetic input. In that case detection can work, the preview can show boxes, and the GUI can open with `Home`, but aim movement or auto-shoot still will not reach the game.

Treat this as an input-chain problem, not a model or capture problem:

1. Confirm boxes or preview detections are visible.
2. Confirm the console prints the expected line, for example `[Mouse] Using WIN32 input.`
3. Test whether movement works on the Windows desktop or in a non-blocking app.
4. If desktop movement works but the game does not react, switch away from `WIN32`.
5. Use a method that matches the device you actually have: `KMBOX_NET`, `KMBOX_A`, or `MAKCU`.

For games that block the standard Win32 path, a separate supported input device is usually required. KMBOX and MAKCU send movement through an external bridge instead of relying on normal Windows synthetic mouse events. The app does not create this device for you; it must be connected, configured, and visible to the selected `input_method`.

Related docs:

- [Input method config](../config.md#input-method)
- [Common recipes](recipes.md)
