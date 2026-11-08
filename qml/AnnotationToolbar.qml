// SPDX-FileCopyrightText: 2026 Andy Stewart
// SPDX-License-Identifier: GPL-3.0-only

import QtQuick

Item {
    id: root
    property bool longImage: false
    property bool toolbarVisible: true
    property rect selectionRect: Qt.rect(0, 0, 0, 0)
    property alias colorPanelVisible: colorPanel.visible
    property alias variantPanelVisible: variantPanel.visible
    readonly property int buttonCount: toolbarRepeater.count
    readonly property real toolbarHeight: toolbar.height
    readonly property int annotationCount: captureController.annotations.length
    property string toolbarTooltipText: ""
    property real toolbarTooltipX: 0
    property real toolbarTooltipY: 0
    property bool toolbarTooltipVisible: false
    // While text is being edited the tooltip shows editing keys beside it.
    property bool editing: false
    property rect editorRect: Qt.rect(0, 0, 0, 0)
    signal commitRequested()
    signal focusRequested()
    signal zoomRequested(real factor)

    readonly property var actions: [
        {key: "V", action: "select", hint: qsTr("Selection · Arrow keys expand, Shift+arrows shrink (1 px)")},
        {key: "R", action: "rect_group", hint: qsTr("Rectangle")},
        {key: "E", action: "ellipse_group", hint: qsTr("Ellipse")},
        {key: "A", action: "arrow_group", hint: qsTr("Arrow")},
        {key: "D", action: "pen_group", hint: qsTr("Pen")},
        {key: "T", action: "text", hint: qsTr("Text · Click to type")},
        {key: "G", action: "mosaic", hint: qsTr("Mosaic · Drag to redact")},
        {key: "B", action: "marker", hint: qsTr("Number · Click to add")}
    ].concat(longImage ? [
        {key: "-", action: "zoom_out", hint: qsTr("Zoom out")},
        {key: "=", action: "zoom_in", hint: qsTr("Zoom in")},
        {key: "H", action: "resume", hint: qsTr("Continue capture · Keeps annotations")}
    ] : [
        {key: "H", action: "scroll", hint: qsTr("Scrolling capture")},
        {key: "O", action: "ocr", hint: qsTr("OCR · Recognize text")}
    ]).concat([
        {key: "Z", action: "undo", hint: qsTr("Undo")},
        {key: "X", action: "redo", hint: qsTr("Redo")},
        {key: "P", action: "pin", hint: qsTr("Pin screenshot")},
        {key: "C", action: "copy", hint: qsTr("Copy screenshot")},
        {key: "S", action: "save", hint: qsTr("Save PNG")}
    ])
    readonly property real contentWidth: 64 + actions.reduce(
        (total, action) => total + (buttonWidth(action) > 0 ? buttonWidth(action) + 2 : 0), 0)

    function buttonWidth(action) {
        if (action.action === "scroll" && annotationCount > 0) return 0
        return action.key.length > 0 ? 64 : 36
    }

    readonly property bool darkToolbar: captureController.darkToolbar
    readonly property color toolbarSurface: darkToolbar ? "#f0222b36" : "#f8ffffff"
    readonly property color panelSurface: darkToolbar ? "#222b36" : "#ffffff"
    readonly property color panelBorder: darkToolbar ? "#526171" : "#cbd4de"
    readonly property color toolbarInk: darkToolbar ? "#e9f0f6" : "#26313d"
    readonly property color toolbarHover: darkToolbar ? "#354352" : "#edf3f9"
    readonly property color toolbarSelected: darkToolbar ? "#315577" : "#dcecff"

    function isToolGroup(action) { return action.endsWith("_group") }
    function groupName(action) { return action.substring(0, action.length - 6) }
    function displayedTool(action) {
        return isToolGroup(action)
               ? captureController.toolVariants[groupName(action)] : action
    }

    function toolGroupOptions(group) {
        switch (group) {
        case "rect": return [
            {action: "rect", label: qsTr("Rectangle")},
            {action: "roundrect", label: qsTr("Rounded rectangle")},
            {action: "fillrect", label: qsTr("Filled rectangle")}]
        case "ellipse": return [
            {action: "ellipse", label: qsTr("Ellipse")},
            {action: "fillellipse", label: qsTr("Filled ellipse")},
            {action: "spotlight", label: qsTr("Spotlight")}]
        case "arrow": return [
            {action: "arrow", label: qsTr("Arrow")},
            {action: "curvedarrow", label: qsTr("Curved arrow")},
            {action: "doublearrow", label: qsTr("Double-headed curved arrow")},
            {action: "line", label: qsTr("Line")}]
        case "pen": return [
            {action: "pen", label: qsTr("Pen")},
            {action: "highlighter", label: qsTr("Highlighter")}]
        }
        return []
    }

    function toolbarButtonCenter(index) {
        let button = toolbarRepeater.itemAt(index)
        if (button === null) return {x: -1, y: -1}
        let point = button.mapToItem(root, button.width / 2, button.height / 2)
        return {x: point.x, y: point.y}
    }

    function toolbarDisplayedTool(index) {
        let button = toolbarRepeater.itemAt(index)
        return button === null ? "" : root.displayedTool(button.modelData.action)
    }

    function colorPresetCenter(index) {
        let swatch = colorPresetRepeater.itemAt(index)
        if (swatch === null) return {x: -1, y: -1}
        let point = swatch.mapToItem(root, swatch.width / 2, swatch.height / 2)
        return {x: point.x, y: point.y}
    }

    function themeButtonCenter(index) {
        let button = themeRepeater.itemAt(index)
        if (button === null) return {x: -1, y: -1}
        let point = button.mapToItem(root, button.width / 2, button.height / 2)
        return {x: point.x, y: point.y}
    }

    function variantOptionCenter(index) {
        let option = variantOptionRepeater.itemAt(index)
        if (option === null) return {x: -1, y: -1}
        let point = option.mapToItem(root, option.width / 2, option.height / 2)
        return {x: point.x, y: point.y}
    }

    function toolbarHasAction(action) {
        for (let i = 0; i < toolbarRepeater.count; ++i) {
            let button = toolbarRepeater.itemAt(i)
            if (button !== null && button.modelData.action === action) return true
        }
        return false
    }

    function toolbarActionCenter(action) {
        for (let i = 0; i < toolbarRepeater.count; ++i) {
            let button = toolbarRepeater.itemAt(i)
            if (button !== null && button.modelData.action === action)
                return toolbarButtonCenter(i)
        }
        return {x: -1, y: -1}
    }

    function showToolbarTooltip(hint, button) {
        let point = button.mapToItem(root, button.width / 2, 0)
        toolbarTooltipText = hint
        toolbarTooltipX = point.x
        toolbarTooltipY = point.y
        toolbarTooltipVisible = false
        tooltipDelay.restart()
    }

    function hideToolbarTooltip() {
        tooltipDelay.stop()
        toolbarTooltipVisible = false
    }

    // Closes popups left open by the previous capture in a resident process.
    function closePopups() {
        colorPanel.visible = false
        variantPanel.visible = false
        hideToolbarTooltip()
    }

    Timer {
        id: tooltipDelay
        interval: 450
        onTriggered: root.toolbarTooltipVisible = true
    }

    Connections {
        target: captureController
        function onDraftChanged() {
            if (variantPanel.visible && captureController.draft.type)
                variantPanel.visible = false
        }
    }

    function handleShortcut(event) {
        if (colorPanel.visible && event.key === Qt.Key_Q) {
            colorPanel.visible = false
            root.focusRequested()
            event.accepted = true
            return true
        }
        const closeColorPanel = colorPanel.visible
        if (variantPanel.visible) {
            variantPanel.visible = false
        }
        if (captureController.selected && event.key === Qt.Key_H &&
                 event.modifiers === Qt.NoModifier) {
            if (!root.longImage && root.annotationCount > 0) return false
            root.commitRequested()
            if (root.longImage) captureController.resumeScroll()
            else captureController.startScroll()
        } else if (root.longImage && captureController.selected &&
                 ((event.key === Qt.Key_Equal && event.modifiers === Qt.NoModifier) ||
                  (event.key === Qt.Key_Plus &&
                   (event.modifiers === Qt.NoModifier || event.modifiers === Qt.ShiftModifier))))
            root.zoomRequested(1.25)
        else if (root.longImage && captureController.selected &&
                 event.key === Qt.Key_Minus && event.modifiers === Qt.NoModifier)
            root.zoomRequested(0.8)
        else if (!root.longImage && captureController.selected &&
                 event.key === Qt.Key_O && event.modifiers === Qt.NoModifier) {
            root.commitRequested()
            captureController.startOcr()
        }
        else if (captureController.selected && captureController.tool === "select" &&
                 (event.key === Qt.Key_Up || event.key === Qt.Key_Down ||
                  event.key === Qt.Key_Left || event.key === Qt.Key_Right) &&
                 (event.modifiers === Qt.NoModifier || event.modifiers === Qt.ShiftModifier))
            captureController.adjustSelectionEdge(event.key, (event.modifiers & Qt.ShiftModifier) !== 0)
        else if (captureController.selected && event.key === Qt.Key_C) { root.commitRequested(); captureController.copy() }
        else if (captureController.selected && event.key === Qt.Key_S) { root.commitRequested(); captureController.save() }
        else if (captureController.selected && event.key === Qt.Key_P &&
                 event.modifiers === Qt.NoModifier) { root.commitRequested(); captureController.pin() }
        else if (captureController.selected && event.key === Qt.Key_X &&
                 event.modifiers === Qt.NoModifier) captureController.redo()
        else if (captureController.selected && event.key === Qt.Key_Z &&
                 event.modifiers === Qt.NoModifier) captureController.undo()
        else if (captureController.selected && event.key === Qt.Key_Q) colorPanel.visible = true
        else if (captureController.selected && event.key === Qt.Key_V) captureController.tool = "select"
        else if (captureController.selected && event.key === Qt.Key_R &&
                 (event.modifiers & Qt.ShiftModifier)) captureController.tool = "roundrect"
        else if (captureController.selected && event.key === Qt.Key_E &&
                 (event.modifiers & Qt.ShiftModifier)) captureController.tool = "fillellipse"
        else if (captureController.selected && event.key === Qt.Key_A &&
                 (event.modifiers & Qt.ShiftModifier)) captureController.tool = "curvedarrow"
        else if (captureController.selected && event.key === Qt.Key_D &&
                 (event.modifiers & Qt.ShiftModifier)) captureController.tool = "fillrect"
        else if (captureController.selected && event.key === Qt.Key_B &&
                 (event.modifiers & Qt.ShiftModifier)) captureController.tool = "spotlight"
        else if (captureController.selected && event.key === Qt.Key_W &&
                 (event.modifiers & Qt.ShiftModifier)) captureController.tool = "doublearrow"
        else if (captureController.selected && event.key === Qt.Key_W) captureController.tool = "line"
        else if (captureController.selected && event.key === Qt.Key_R) captureController.activateToolGroup("rect")
        else if (captureController.selected && event.key === Qt.Key_E) captureController.activateToolGroup("ellipse")
        else if (captureController.selected && event.key === Qt.Key_A) captureController.activateToolGroup("arrow")
        else if (captureController.selected && event.key === Qt.Key_D) captureController.activateToolGroup("pen")
        else if (captureController.selected && event.key === Qt.Key_T &&
                 event.modifiers === Qt.NoModifier) captureController.tool = "text"
        else if (captureController.selected && event.key === Qt.Key_G &&
                 event.modifiers === Qt.NoModifier) captureController.tool = "mosaic"
        else if (captureController.selected && event.key === Qt.Key_B &&
                 event.modifiers === Qt.NoModifier) captureController.tool = "marker"
        else return false
        if (closeColorPanel) colorPanel.visible = false
        event.accepted = true
        return true
    }

    Rectangle {
        id: toolbar
        objectName: root.longImage ? "longToolbar" : "toolbar"
        visible: root.toolbarVisible
        onVisibleChanged: if (!visible) {
            root.hideToolbarTooltip()
            colorPanel.visible = false
            variantPanel.visible = false
        }
        z: 10
        width: toolsRow.width + 12
        height: toolsRow.height + 10
        radius: 6
        color: root.toolbarSurface
        border.color: root.panelBorder
        x: root.longImage ? (root.width - width) / 2 :
           Math.max(8, Math.min(root.selectionRect.x + root.selectionRect.width - width,
                                root.width - width - 8))
        y: root.longImage ? root.height - height - 12 :
           root.selectionRect.y + root.selectionRect.height + height + 8 < root.height
           ? root.selectionRect.y + root.selectionRect.height + 8
           : Math.max(8, root.selectionRect.y - height - 8)

        Flow {
            id: toolsRow
            anchors.centerIn: parent
            spacing: 2
            width: Math.max(64, Math.min(root.width - 28, root.contentWidth))
            height: childrenRect.height
            Rectangle {
                id: colorButton
                objectName: "colorButton"
                width: 64
                height: 36
                radius: 4
                color: "transparent"

                Row {
                    anchors.centerIn: parent
                    spacing: 2
                    Text {
                        text: "Q:"
                        width: 25
                        height: 20
                        color: root.toolbarInk
                        font.pixelSize: 14
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    ToolbarGlyph {
                        action: "color"
                        accentColor: captureController.annotationColor
                        darkMode: root.darkToolbar
                    }
                }
                MouseArea {
                    id: colorButtonMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onEntered: root.showToolbarTooltip(qsTr("Color"), colorButtonMouse)
                    onExited: root.hideToolbarTooltip()
                    onClicked: {
                        root.hideToolbarTooltip()
                        root.commitRequested()
                        variantPanel.visible = false
                        colorPanel.visible = !colorPanel.visible
                        root.focusRequested()
                    }
                }
            }
            Repeater {
                id: toolbarRepeater
                model: root.actions
                delegate: Rectangle {
                    required property var modelData
                    objectName: (root.longImage ? "longTool_" : "tool_") + modelData.action
                    readonly property bool selected: captureController.tool === root.displayedTool(modelData.action)
                    Accessible.name: modelData.hint
                    width: root.buttonWidth(modelData)
                    height: 36
                    visible: width > 0
                    radius: 4
                    color: "transparent"
                    Rectangle {
                        objectName: root.longImage ? "longHighlight_" + modelData.action :
                                    modelData.action === "select" ? "selectHighlight" : ""
                        anchors.centerIn: parent
                        width: Math.max(0, parent.width - 4)
                        height: 30
                        radius: 5
                        color: parent.selected ? root.toolbarSelected :
                               button.containsMouse ? root.toolbarHover : "transparent"
                    }
                    Row {
                        objectName: modelData.action === "select" ? "selectContent" : ""
                        anchors.centerIn: parent
                        spacing: 2
                        Text {
                            objectName: (root.longImage ? "longLabel_" : "toolLabel_") + modelData.action
                            text: modelData.key.length > 0 ? modelData.key + ":" : ""
                            visible: modelData.key.length > 0
                            width: visible ? 25 : 0
                            height: 20
                            color: root.toolbarInk
                            font.pixelSize: 14
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        ToolbarGlyph {
                            objectName: root.longImage ? "longGlyph_" + modelData.action :
                                        modelData.action === "select" ? "selectCursorGlyph" :
                                        "toolGlyph_" + modelData.action
                            action: root.displayedTool(modelData.action)
                            accentColor: captureController.annotationColor
                            darkMode: root.darkToolbar
                        }
                    }
                    MouseArea {
                        id: button
                        anchors.fill: parent
                        hoverEnabled: true
                        onEntered: root.showToolbarTooltip(modelData.hint, button)
                        onExited: root.hideToolbarTooltip()
                        onClicked: {
                            root.hideToolbarTooltip()
                            root.commitRequested()
                            colorPanel.visible = false
                            if (root.isToolGroup(modelData.action)) {
                                variantPanel.openFor(root.groupName(modelData.action), button)
                            } else {
                                variantPanel.visible = false
                                switch (modelData.action) {
                                case "undo": captureController.undo(); break
                                case "copy": captureController.copy(); break
                                case "save": captureController.save(); break
                                case "pin": captureController.pin(); break
                                case "redo": captureController.redo(); break
                                case "scroll": captureController.startScroll(); break
                                case "resume": captureController.resumeScroll(); break
                                case "ocr": captureController.startOcr(); break
                                case "zoom_out": root.zoomRequested(0.8); break
                                case "zoom_in": root.zoomRequested(1.25); break
                                default: captureController.tool = modelData.action
                                }
                            }
                            root.focusRequested()
                        }
                    }
                }
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        z: 9
        visible: colorPanel.visible
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: {
            colorPanel.visible = false
            variantPanel.visible = false
        }
    }

    Rectangle {
        id: variantPanel
        objectName: "variantPanel"
        property string group: ""
        property real anchorX: 0
        readonly property var options: root.toolGroupOptions(group)
        visible: false
        z: 20
        width: options.length * 38 + 10
        height: 48
        radius: 7
        color: root.panelSurface
        border.color: root.panelBorder
        x: Math.max(8, Math.min(anchorX - width / 2, root.width - width - 8))
        y: toolbar.y - height - 8 >= 8
           ? toolbar.y - height - 8
           : toolbar.y + toolbar.height + 8

        function openFor(chosenGroup, button) {
            const wasOpen = visible && group === chosenGroup
            group = chosenGroup
            let point = button.mapToItem(root, button.width / 2, 0)
            anchorX = point.x
            captureController.activateToolGroup(chosenGroup)
            visible = !wasOpen
        }

        Row {
            anchors.centerIn: parent
            spacing: 2
            Repeater {
                id: variantOptionRepeater
                model: variantPanel.options
                delegate: Rectangle {
                    required property var modelData
                    width: 36
                    height: 36
                    color: "transparent"
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 3
                        radius: 5
                        color: captureController.tool === modelData.action
                               ? root.toolbarSelected : optionMouse.containsMouse ? root.toolbarHover : "transparent"
                    }
                    ToolbarGlyph {
                        anchors.centerIn: parent
                        action: modelData.action
                        accentColor: captureController.annotationColor
                        darkMode: root.darkToolbar
                    }
                    MouseArea {
                        id: optionMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onEntered: root.showToolbarTooltip(modelData.label, optionMouse)
                        onExited: root.hideToolbarTooltip()
                        onClicked: {
                            root.hideToolbarTooltip()
                            captureController.tool = modelData.action
                            variantPanel.visible = false
                            root.focusRequested()
                        }
                    }
                }
            }
        }
    }

    Rectangle {
        id: colorPanel
        objectName: "colorPanel"
        property real hue: 0
        property real saturation: 1
        property real brightness: 1
        visible: false
        z: 20
        width: 260
        height: 246
        radius: 7
        color: root.panelSurface
        border.color: root.panelBorder
        x: Math.max(8, Math.min(toolbar.x, root.width - width - 8))
        y: toolbar.y - height - 8 >= 8
           ? toolbar.y - height - 8
           : toolbar.y + toolbar.height + 8

        function loadColor() {
            let color = captureController.annotationColor
            let r = parseInt(color.slice(1, 3), 16) / 255
            let g = parseInt(color.slice(3, 5), 16) / 255
            let b = parseInt(color.slice(5, 7), 16) / 255
            let highest = Math.max(r, g, b)
            let lowest = Math.min(r, g, b)
            let delta = highest - lowest
            let h = 0
            if (delta > 0) {
                if (highest === r) h = ((g - b) / delta) % 6
                else if (highest === g) h = (b - r) / delta + 2
                else h = (r - g) / delta + 4
                h = (h / 6 + 1) % 1
            }
            hue = h
            saturation = highest === 0 ? 0 : delta / highest
            brightness = highest
        }

        function chooseColor(h, s, v) {
            hue = Math.max(0, Math.min(1, h))
            saturation = Math.max(0, Math.min(1, s))
            brightness = Math.max(0, Math.min(1, v))
            captureController.setAnnotationColorFromHsv(hue, saturation, brightness)
        }

        onVisibleChanged: {
            if (visible) {
                variantPanel.visible = false
                loadColor()
                root.hideToolbarTooltip()
            } else captureController.saveAnnotationColor()
        }
        onHueChanged: saturationValueField.requestPaint()

        Column {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 10

            Item {
                width: 238
                height: 26
                Row {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 4
                    Repeater {
                        id: themeRepeater
                        model: [
                            {action: "theme_light", dark: false, hint: qsTr("Light")},
                            {action: "theme_dark", dark: true, hint: qsTr("Dark")}
                        ]
                        delegate: Rectangle {
                            required property var modelData
                            objectName: modelData.dark ? "darkThemeButton" : "lightThemeButton"
                            width: 28
                            height: 26
                            radius: 5
                            color: root.darkToolbar === modelData.dark
                                   ? root.toolbarSelected
                                   : themeButtonMouse.containsMouse ? root.toolbarHover : "transparent"
                            ToolbarGlyph {
                                anchors.centerIn: parent
                                width: 16
                                height: 16
                                action: modelData.action
                                darkMode: root.darkToolbar
                            }
                            MouseArea {
                                id: themeButtonMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onEntered: root.showToolbarTooltip(modelData.hint, themeButtonMouse)
                                onExited: root.hideToolbarTooltip()
                                onClicked: {
                                    root.hideToolbarTooltip()
                                    captureController.darkToolbar = modelData.dark
                                }
                            }
                        }
                    }
                }
                Rectangle {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: 18
                    height: 18
                    radius: 9
                    color: captureController.annotationColor
                    border.color: root.panelBorder
                }
            }

            Row {
                spacing: 10
                Canvas {
                    id: saturationValueField
                    objectName: "saturationValueField"
                    width: 208
                    height: 160
                    antialiasing: true
                    onPaint: {
                        let context = getContext("2d")
                        context.clearRect(0, 0, width, height)
                        context.fillStyle = Qt.hsla(colorPanel.hue, 1, 0.5, 1)
                        context.fillRect(0, 0, width, height)
                        let white = context.createLinearGradient(0, 0, width, 0)
                        white.addColorStop(0, "#ffffff")
                        white.addColorStop(1, "rgba(255, 255, 255, 0)")
                        context.fillStyle = white
                        context.fillRect(0, 0, width, height)
                        let black = context.createLinearGradient(0, 0, 0, height)
                        black.addColorStop(0, "rgba(0, 0, 0, 0)")
                        black.addColorStop(1, "#000000")
                        context.fillStyle = black
                        context.fillRect(0, 0, width, height)
                    }
                    MouseArea {
                        anchors.fill: parent
                        function pick(mouse) {
                            colorPanel.chooseColor(colorPanel.hue,
                                                   mouse.x / width,
                                                   1 - mouse.y / height)
                        }
                        onPressed: mouse => pick(mouse)
                        onPositionChanged: mouse => { if (pressed) pick(mouse) }
                        onReleased: captureController.saveAnnotationColor()
                    }
                    Rectangle {
                        width: 14
                        height: 14
                        radius: 7
                        x: colorPanel.saturation * saturationValueField.width - width / 2
                        y: (1 - colorPanel.brightness) * saturationValueField.height - height / 2
                        color: "transparent"
                        border.color: "#26313d"
                        border.width: 1
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 2
                            radius: 5
                            color: "transparent"
                            border.color: "white"
                            border.width: 2
                        }
                    }
                }
                Canvas {
                    id: hueField
                    objectName: "hueField"
                    width: 20
                    height: 160
                    onPaint: {
                        let context = getContext("2d")
                        let gradient = context.createLinearGradient(0, 0, 0, height)
                        gradient.addColorStop(0, "#ff0000")
                        gradient.addColorStop(1 / 6, "#ffff00")
                        gradient.addColorStop(2 / 6, "#00ff00")
                        gradient.addColorStop(3 / 6, "#00ffff")
                        gradient.addColorStop(4 / 6, "#0000ff")
                        gradient.addColorStop(5 / 6, "#ff00ff")
                        gradient.addColorStop(1, "#ff0000")
                        context.fillStyle = gradient
                        context.fillRect(0, 0, width, height)
                    }
                    MouseArea {
                        anchors.fill: parent
                        function pick(mouse) {
                            colorPanel.chooseColor(mouse.y / height,
                                                   colorPanel.saturation,
                                                   colorPanel.brightness)
                        }
                        onPressed: mouse => pick(mouse)
                        onPositionChanged: mouse => { if (pressed) pick(mouse) }
                        onReleased: captureController.saveAnnotationColor()
                    }
                    Rectangle {
                        x: -2
                        y: colorPanel.hue * hueField.height - height / 2
                        width: 24
                        height: 6
                        radius: 2
                        color: "transparent"
                        border.color: "#26313d"
                        border.width: 1
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 1
                            radius: 1
                            color: "transparent"
                            border.color: "white"
                            border.width: 1
                        }
                    }
                }
            }

            Row {
                spacing: 5
                Repeater {
                    id: colorPresetRepeater
                    model: ["#ff4b55", "#ff9d42", "#ffd84d", "#5bd686", "#42c8d8",
                            "#4d94ff", "#a77bff", "#f774ba", "#ffffff", "#202020"]
                    delegate: Rectangle {
                        required property string modelData
                        width: 18
                        height: 18
                        radius: 4
                        color: modelData
                        border.color: captureController.annotationColor === modelData
                                      ? root.toolbarInk : root.panelBorder
                        border.width: captureController.annotationColor === modelData ? 2 : 1
                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                captureController.annotationColor = modelData
                                colorPanel.visible = false
                                root.focusRequested()
                            }
                        }
                    }
                }
            }
        }
    }

    Rectangle {
        id: toolbarTooltip
        objectName: "toolbarTooltip"
        visible: (root.toolbarTooltipVisible && toolbar.visible) || root.editing
        z: 30
        width: Math.min(tooltipLabel.implicitWidth + 20, root.width - 16)
        height: tooltipLabel.implicitHeight + 12
        readonly property real anchorX: root.editing ? root.editorRect.x + root.editorRect.width / 2 : root.toolbarTooltipX
        readonly property real anchorY: root.editing ? root.editorRect.y : root.toolbarTooltipY
        x: Math.max(8, Math.min(anchorX - width / 2,
                                root.width - width - 8))
        y: anchorY - height - 8 >= 8
           ? anchorY - height - 8
           : anchorY + (root.editing ? root.editorRect.height + 8 : 40)
        radius: 5
        color: "#f0202734"
        border.color: "#748090"
        Text {
            id: tooltipLabel
            objectName: "tooltipLabel"
            anchors.centerIn: parent
            width: parent.width - 20
            text: root.editing
                  ? qsTr("Enter: confirm · Shift+Enter: new line · Alt: confirm & return to previous tool")
                  : root.toolbarTooltipText
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            horizontalAlignment: Qt.application.layoutDirection === Qt.RightToLeft
                                 ? Text.AlignRight : Text.AlignLeft
            color: "white"
            font.pixelSize: 13
        }
    }

}
