import "." as Skin
import Mixxx 1.0 as Mixxx
import QtQuick 2.15

Skin.Fader {
    id: root

    required property string group
    required property string key

    // Do not push control updates (sync, MIDI) into the handle mid-drag: they
    // fight the finger and make the slider look dead. The handle re-syncs on release.
    Binding {
        restoreMode: Binding.RestoreNone
        target: root
        property: "value"
        value: control.parameter
        when: !root.pressed
    }

    onMoved: function (value) {
        control.parameter = value;
    }

    Mixxx.ControlProxy {
        id: control

        group: root.group
        key: root.key
    }
    TapHandler {
        onDoubleTapped: control.reset()
    }
    TapHandler {
        acceptedButtons: Qt.RightButton

        onTapped: control.reset()
    }
}
