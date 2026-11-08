// SPDX-FileCopyrightText: 2026 Andy Stewart
// SPDX-License-Identifier: GPL-3.0-only

import QtQuick

Item {
    id: root
    clip: true
    focus: true
    property int screenIndex: -1
    property rect screenRect: Qt.rect(0, 0, 0, 0)

    readonly property var activeRect: captureController.selection.width > 0
                                      ? captureController.selection : captureController.hovered
    readonly property int annotationCount: captureController.annotations.length
    readonly property bool captureReady: screenImage.status === Image.Ready
    readonly property bool editorFocused: textInput.activeFocus
    readonly property bool mosaicDraftBorderVisible: mosaicDraftBorder.visible
    readonly property int toolbarButtonCount: annotationToolbar.buttonCount
    readonly property bool toolbarTooltipVisible: annotationToolbar.toolbarTooltipVisible
    readonly property string toolbarTooltipText: annotationToolbar.toolbarTooltipText
    readonly property string editorText: editor.visible ? textInput.text : ""
    readonly property int editorRows: editor.visible ? textInput.lineCount : 0
    readonly property real textLineHeight: textMetrics.lineSpacing
    readonly property real localX: activeRect.x - screenRect.x
    readonly property real localY: activeRect.y - screenRect.y
    readonly property real holeLeft: activeRect.width > 0 ? Math.max(0, Math.min(width, localX)) : 0
    readonly property real holeTop: activeRect.height > 0 ? Math.max(0, Math.min(height, localY)) : 0
    readonly property real holeRight: activeRect.width > 0 ? Math.max(0, Math.min(width, localX + activeRect.width)) : 0
    readonly property real holeBottom: activeRect.height > 0 ? Math.max(0, Math.min(height, localY + activeRect.height)) : 0
    property real textX: 0
    property real textY: 0

    FontMetrics {
        id: textMetrics
        font.pixelSize: 22
        font.bold: true
    }

    function previewState() {
        let textItem = textRepeater.itemAt(1)
        return {
            mosaicExists: mosaicOverlay.hasMosaic,
            mosaicVisible: mosaicOverlay.visible && mosaicOverlay.hasMosaic,
            mosaicWidth: mosaicOverlay.width,
            mosaicImageReady: mosaicOverlay.imageReady,
            textExists: textItem !== null,
            textVisible: textItem !== null && textItem.visible,
            textValue: textItem !== null ? textItem.text : "",
            textRows: textItem !== null ? textItem.lines.length : 0,
            firstLineCenter: textItem !== null ? textItem.lineCenter(0) : -1,
            secondLineCenter: textItem !== null ? textItem.lineCenter(1) : -1,
            textLineHeight: root.textLineHeight
        }
    }

    function toolbarButtonCenter(index) { return annotationToolbar.toolbarButtonCenter(index) }
    function toolbarDisplayedTool(index) { return annotationToolbar.toolbarDisplayedTool(index) }
    function colorPresetCenter(index) { return annotationToolbar.colorPresetCenter(index) }
    function themeButtonCenter(index) { return annotationToolbar.themeButtonCenter(index) }
    function variantOptionCenter(index) { return annotationToolbar.variantOptionCenter(index) }

    function copySelectionAt(localX, localY) {
        const x = screenRect.x + localX
        const y = screenRect.y + localY
        const selection = captureController.selection
        if (!captureController.selected ||
            x <= selection.x || y <= selection.y ||
            x >= selection.x + selection.width ||
            y >= selection.y + selection.height)
            return false
        const removeMarker = captureController.tool === "marker" && picker.lastMarkerClickAdded
        picker.lastMarkerClickAdded = false
        if (editor.visible) editor.commitText()
        root.forceActiveFocus()
        captureController.cancelPointerAction()
        if (removeMarker) captureController.undo()
        captureController.copy()
        return true
    }

    function visibleResizeHandleCount() {
        let count = 0
        for (let i = 0; i < resizeHandles.count; ++i) {
            let handle = resizeHandles.itemAt(i)
            if (handle !== null && handle.visible) ++count
        }
        return count
    }

    function toolbarHasAction(action) { return annotationToolbar.toolbarHasAction(action) }

    // A resident process reuses this overlay; drop transient UI from the last capture.
    function resetForCapture() {
        editor.visible = false
        textInput.text = ""
        // Otherwise a double-click copy could undo the next capture's edit.
        picker.lastMarkerClickAdded = false
        annotationToolbar.closePopups()
        root.forceActiveFocus()
    }

    function leaveTextTool() {
        if (captureController.tool !== "text") return
        if (editor.visible) editor.commitText()
        captureController.restoreToolBeforeText()
        root.forceActiveFocus()
    }

    Keys.onPressed: event => {
        if (event.key === Qt.Key_Escape) {
            if (captureController.ocrVisible)
                captureController.closeOcr()
            else if (captureController.scrollState === 1)
                captureController.cancelScrollChoice()
            else
                captureController.cancel()
            event.accepted = true
            return
        }
        if (captureController.ocrVisible) {
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Alt && captureController.tool === "text" &&
            (event.modifiers === Qt.NoModifier || event.modifiers === Qt.AltModifier)) {
            root.leaveTextTool()
            event.accepted = true
            return
        }
        annotationToolbar.handleShortcut(event)
    }

    Image {
        id: screenImage
        objectName: "screenCaptureImage"
        anchors.fill: parent
        source: screenIndex >= 0 && captureController.imagesReady
                ? "image://captures/screen/" + screenIndex + "/" + captureController.captureGeneration : ""
        fillMode: Image.Stretch
        smooth: true
        cache: true
    }

    Rectangle { x: 0; y: 0; width: root.width; height: root.holeTop; color: "#85000000" }
    Rectangle { x: 0; y: root.holeBottom; width: root.width; height: root.height - y; color: "#85000000" }
    Rectangle { x: 0; y: root.holeTop; width: root.holeLeft; height: root.holeBottom - y; color: "#85000000" }
    Rectangle { x: root.holeRight; y: root.holeTop; width: root.width - x; height: root.holeBottom - y; color: "#85000000" }

    Rectangle {
        objectName: "selectionBorder"
        visible: root.activeRect.width > 0 && root.activeRect.height > 0
        x: root.localX
        y: root.localY
        width: root.activeRect.width
        height: root.activeRect.height
        color: "transparent"
        border.color: captureController.selected ? "#ffffff" : "#59bdff"
        border.width: 2
    }

    // Each patch shows the coarse capture stretched without smoothing, so the
    // GPU pixelates it and dragging a mosaic never repaints on the CPU.
    component MosaicPatch: Item {
        id: patch
        property var mark
        property rect screen
        property string source
        readonly property bool active: !!mark && mark.type === "mosaic" && !!mark.start && !!mark.end
        visible: active
        x: active ? Math.min(mark.start.x, mark.end.x) - screen.x : 0
        y: active ? Math.min(mark.start.y, mark.end.y) - screen.y : 0
        width: active ? Math.abs(mark.end.x - mark.start.x) : 0
        height: active ? Math.abs(mark.end.y - mark.start.y) : 0
        clip: true
        Image {
            x: -patch.x
            y: -patch.y
            width: patch.screen.width
            height: patch.screen.height
            source: patch.active ? patch.source : ""
            smooth: false
            cache: true
        }
    }

    Item {
        id: mosaicOverlay
        objectName: "mosaicOverlay"
        anchors.fill: parent
        readonly property bool hasMosaic: draftPatch.active ||
            captureController.annotations.some(item => item.type === "mosaic")
        readonly property bool imageReady: captureController.imagesReady
        readonly property string source: root.screenIndex >= 0 && captureController.imagesReady
                                         ? "image://captures/mosaic/" + root.screenIndex + "/"
                                           + captureController.captureGeneration : ""
        visible: captureController.selected && hasMosaic

        Item {
            x: root.holeLeft
            y: root.holeTop
            width: root.holeRight - root.holeLeft
            height: root.holeBottom - root.holeTop
            clip: true
            Item {
                x: -parent.x
                y: -parent.y
                width: root.width
                height: root.height
                Repeater {
                    model: captureController.annotations.length
                    delegate: MosaicPatch {
                        required property int index
                        mark: captureController.annotations[index]
                        screen: root.screenRect
                        source: mosaicOverlay.source
                    }
                }
                MosaicPatch {
                    id: draftPatch
                    mark: captureController.draft
                    screen: root.screenRect
                    source: mosaicOverlay.source
                }
            }
        }
    }

    Rectangle {
        id: mosaicDraftBorder
        objectName: "mosaicDraftBorder"
        readonly property var mark: captureController.draft
        readonly property bool active: mark.type === "mosaic" && mark.start && mark.end
        x: active ? Math.min(mark.start.x, mark.end.x) - screenRect.x : 0
        y: active ? Math.min(mark.start.y, mark.end.y) - screenRect.y : 0
        width: active ? Math.abs(mark.end.x - mark.start.x) : 0
        height: active ? Math.abs(mark.end.y - mark.start.y) : 0
        visible: active && width >= 2 && height >= 2
        color: "transparent"
        border.color: captureController.annotationColor
        border.width: 2
    }

    // Committed marks, clipped to the selection. Canvas rasterizes on the GUI
    // thread, so it repaints only when the marks or the selection change.
    Canvas {
        id: marks
        objectName: "marksCanvas"
        readonly property bool spotlightDraft: captureController.draft.type === "spotlight"
        readonly property bool hasContent: captureController.annotations.length > 0 || spotlightDraft
        property bool painted: false
        x: root.holeLeft
        y: root.holeTop
        width: root.holeRight - root.holeLeft
        height: root.holeBottom - root.holeTop
        visible: captureController.selected && hasContent && width > 0 && height > 0
        antialiasing: true

        function refresh() {
            // An invisible Canvas still rasterizes on requestPaint().
            if (hasContent || painted) requestPaint()
        }
        onWidthChanged: refresh()
        onHeightChanged: refresh()

        function drawShape(context, item) {
            if (!item || !item.type) return
            if (item.type === "mosaic" || item.type === "text") return
            let sx = item.start.x - screenRect.x
            let sy = item.start.y - screenRect.y
            let ex = item.end ? item.end.x - screenRect.x : sx
            let ey = item.end ? item.end.y - screenRect.y : sy
            context.save()
            context.strokeStyle = item.color || "#ff4b55"
            context.fillStyle = item.color || "#ff4b55"
            context.lineWidth = 3
            context.lineCap = "round"
            context.lineJoin = "round"
            context.beginPath()
            if (item.type === "rect") context.rect(sx, sy, ex - sx, ey - sy)
            else if (item.type === "roundrect") {
                let left = Math.min(sx, ex), top = Math.min(sy, ey)
                let w = Math.abs(ex - sx), h = Math.abs(ey - sy)
                let r = Math.min(10, w / 2, h / 2)
                context.moveTo(left + r, top)
                context.lineTo(left + w - r, top)
                context.quadraticCurveTo(left + w, top, left + w, top + r)
                context.lineTo(left + w, top + h - r)
                context.quadraticCurveTo(left + w, top + h, left + w - r, top + h)
                context.lineTo(left + r, top + h)
                context.quadraticCurveTo(left, top + h, left, top + h - r)
                context.lineTo(left, top + r)
                context.quadraticCurveTo(left, top, left + r, top)
                context.closePath()
            }
            else if (item.type === "fillrect") {
                context.fillRect(Math.min(sx, ex), Math.min(sy, ey),
                                 Math.abs(ex - sx), Math.abs(ey - sy))
                context.restore()
                return
            }
            else if (item.type === "fillellipse") {
                context.ellipse(Math.min(sx, ex), Math.min(sy, ey),
                                Math.abs(ex - sx), Math.abs(ey - sy))
                context.fill()
                context.restore()
                return
            }
            else if (item.type === "ellipse") context.ellipse(Math.min(sx, ex), Math.min(sy, ey),
                Math.abs(ex - sx), Math.abs(ey - sy))
            else if (item.type === "line") {
                context.moveTo(sx, sy)
                context.lineTo(ex, ey)
            }
            else if (item.type === "arrow") {
                context.moveTo(sx, sy)
                context.lineTo(ex, ey)
                let a = Math.atan2(ey - sy, ex - sx)
                context.moveTo(ex, ey)
                context.lineTo(ex - Math.cos(a - 0.55) * 12, ey - Math.sin(a - 0.55) * 12)
                context.moveTo(ex, ey)
                context.lineTo(ex - Math.cos(a + 0.55) * 12, ey - Math.sin(a + 0.55) * 12)
            } else if (item.type === "curvedarrow" || item.type === "doublearrow") {
                let cx = (sx + ex) / 2 - (ey - sy) * 0.22
                let cy = (sy + ey) / 2 + (ex - sx) * 0.22
                context.moveTo(sx, sy)
                context.quadraticCurveTo(cx, cy, ex, ey)
                function head(tx, ty, fx, fy) {
                    let angle = Math.atan2(ty - fy, tx - fx)
                    context.moveTo(tx, ty)
                    context.lineTo(tx - Math.cos(angle - 0.55) * 12,
                                   ty - Math.sin(angle - 0.55) * 12)
                    context.moveTo(tx, ty)
                    context.lineTo(tx - Math.cos(angle + 0.55) * 12,
                                   ty - Math.sin(angle + 0.55) * 12)
                }
                head(ex, ey, cx, cy)
                if (item.type === "doublearrow") head(sx, sy, cx, cy)
            } else if (item.type === "marker") {
                context.arc(sx, sy, 13, 0, Math.PI * 2)
                context.fill()
                context.fillStyle = "white"
                context.font = "bold 14px sans-serif"
                context.textAlign = "center"
                context.textBaseline = "middle"
                context.fillText(item.number || "", sx, sy)
                context.restore()
                return
            } else if (item.type === "pen" || item.type === "highlighter") {
                if (item.type === "highlighter") {
                    context.globalAlpha = 0.34
                    context.lineWidth = 18
                }
                let points = captureController.smoothedFreehandPoints(item.points)
                if (points.length === 0) { context.restore(); return }
                context.moveTo(points[0].x - screenRect.x,
                               points[0].y - screenRect.y)
                if (points.length === 2) {
                    context.lineTo(points[1].x - screenRect.x,
                                   points[1].y - screenRect.y)
                } else if (points.length > 2) {
                    for (let i = 1; i + 1 < points.length; ++i) {
                        let control = points[i]
                        let next = points[i + 1]
                        context.quadraticCurveTo(control.x - screenRect.x,
                                                 control.y - screenRect.y,
                                                 (control.x + next.x) / 2 - screenRect.x,
                                                 (control.y + next.y) / 2 - screenRect.y)
                    }
                    let last = points[points.length - 1]
                    context.quadraticCurveTo(last.x - screenRect.x,
                                             last.y - screenRect.y,
                                             last.x - screenRect.x,
                                             last.y - screenRect.y)
                }
            }
            context.stroke()
            context.restore()
        }

        onPaint: {
            let context = getContext("2d")
            context.clearRect(0, 0, width, height)
            painted = hasContent
            if (!hasContent) return
            context.save()
            context.translate(-x, -y)
            context.beginPath()
            context.rect(root.localX, root.localY,
                         captureController.selection.width, captureController.selection.height)
            context.clip()
            let spotlights = []
            for (let item of captureController.annotations)
                if (item.type === "spotlight") spotlights.push(item)
            if (captureController.draft.type === "spotlight")
                spotlights.push(captureController.draft)
            if (spotlights.length > 0) {
                context.save()
                context.fillStyle = "rgba(0, 0, 0, 0.59)"
                context.fillRect(root.localX, root.localY,
                                 captureController.selection.width,
                                 captureController.selection.height)
                context.globalCompositeOperation = "destination-out"
                for (let item of spotlights) {
                    let sx = item.start.x - screenRect.x
                    let sy = item.start.y - screenRect.y
                    let ex = item.end.x - screenRect.x
                    let ey = item.end.y - screenRect.y
                    context.beginPath()
                    context.ellipse(Math.min(sx, ex), Math.min(sy, ey),
                                    Math.abs(ex - sx), Math.abs(ey - sy))
                    context.fill()
                }
                context.restore()
            }
            for (let item of captureController.annotations)
                if (item.type !== "spotlight") drawShape(context, item)
            context.restore()
        }
    }

    // The mark being drawn gets a canvas sized to its own extent, so each
    // pointer move rasterizes only the pixels the shape can touch.
    Canvas {
        id: draftMarks
        objectName: "draftCanvas"
        readonly property var mark: captureController.draft
        readonly property rect bounds: draftBounds(mark)
        x: bounds.x
        y: bounds.y
        width: bounds.width
        height: bounds.height
        visible: captureController.selected && width > 0 && height > 0
        antialiasing: true

        function draftBounds(item) {
            if (!item || !item.type || !item.start || item.type === "mosaic" ||
                item.type === "text" || item.type === "spotlight")
                return Qt.rect(0, 0, 0, 0)
            let points = [item.start, item.end || item.start]
            if (item.points) points = points.concat(item.points)
            if (item.type === "curvedarrow" || item.type === "doublearrow") {
                let end = item.end || item.start
                points.push({x: (item.start.x + end.x) / 2 - (end.y - item.start.y) * 0.22,
                             y: (item.start.y + end.y) / 2 + (end.x - item.start.x) * 0.22})
            }
            // Room for arrowheads, marker discs and the highlighter's width.
            const pad = 20
            let left = Infinity, top = Infinity, right = -Infinity, bottom = -Infinity
            for (let point of points) {
                left = Math.min(left, point.x)
                top = Math.min(top, point.y)
                right = Math.max(right, point.x)
                bottom = Math.max(bottom, point.y)
            }
            left = Math.max(Math.floor(left - pad - screenRect.x), root.holeLeft)
            top = Math.max(Math.floor(top - pad - screenRect.y), root.holeTop)
            right = Math.min(Math.ceil(right + pad - screenRect.x), root.holeRight)
            bottom = Math.min(Math.ceil(bottom + pad - screenRect.y), root.holeBottom)
            return right > left && bottom > top
                   ? Qt.rect(left, top, right - left, bottom - top) : Qt.rect(0, 0, 0, 0)
        }

        onPaint: {
            let context = getContext("2d")
            context.clearRect(0, 0, width, height)
            if (!visible) return
            context.save()
            context.translate(-x, -y)
            marks.drawShape(context, mark)
            context.restore()
        }
    }

    Item {
        x: root.localX
        y: root.localY
        width: captureController.selection.width
        height: captureController.selection.height
        clip: true
        visible: captureController.selected
        Repeater {
            id: textRepeater
            objectName: "textRepeater"
            model: captureController.annotations.length
            delegate: Item {
                objectName: "annotationText"
                required property int index
                readonly property var annotation: captureController.annotations[index]
                visible: annotation && annotation.type === "text"
                x: visible ? annotation.start.x - captureController.selection.x : 0
                y: visible ? annotation.start.y - captureController.selection.y
                             - root.textLineHeight / 2 : 0
                readonly property string text: visible ? annotation.text : ""
                readonly property var lines: text.split("\n")
                width: childrenRect.width
                height: lines.length * root.textLineHeight
                function lineCenter(lineIndex) {
                    let line = lineRepeater.itemAt(lineIndex)
                    return line !== null ? line.y + line.height / 2 : -1
                }
                Repeater {
                    id: lineRepeater
                    model: parent.lines
                    delegate: Text {
                        required property int index
                        required property string modelData
                        y: index * root.textLineHeight
                        height: root.textLineHeight
                        text: modelData
                        color: annotation.color || "#ff4b55"
                        font.bold: true
                        font.pixelSize: 22
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }
        }
    }

    Repeater {
        id: resizeHandles
        model: [
            {horizontal: 0, vertical: 0},
            {horizontal: 0.5, vertical: 0},
            {horizontal: 1, vertical: 0},
            {horizontal: 0, vertical: 0.5},
            {horizontal: 1, vertical: 0.5},
            {horizontal: 0, vertical: 1},
            {horizontal: 0.5, vertical: 1},
            {horizontal: 1, vertical: 1}
        ]
        delegate: SelectionResizeHandle {
            required property var modelData
            objectName: "selectionResizeHandle"
            z: 6
            x: root.localX + captureController.selection.width * modelData.horizontal - width / 2
            y: root.localY + captureController.selection.height * modelData.vertical - height / 2
            visible: captureController.selected && captureController.tool === "select"
        }
    }

    MouseArea {
        id: picker
        property bool lastMarkerClickAdded: false
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: captureController.selected && captureController.tool !== "select"
                     ? Qt.CrossCursor : Qt.ArrowCursor
        onPositionChanged: mouse => captureController.pointerMove(screenIndex, mouse.x, mouse.y)
        onPressed: mouse => {
            if (mouse.button === Qt.RightButton) {
                captureController.cancel()
                return
            }
            if (annotationToolbar.variantPanelVisible) annotationToolbar.variantPanelVisible = false
            root.forceActiveFocus()
            if (captureController.selected && captureController.tool === "text") {
                let gx = screenRect.x + mouse.x
                let gy = screenRect.y + mouse.y
                let rect = captureController.selection
                if (gx >= rect.x && gy >= rect.y && gx <= rect.x + rect.width && gy <= rect.y + rect.height) {
                    root.textX = mouse.x
                    root.textY = mouse.y
                    editor.visible = true
                    textInput.text = ""
                    textInput.forceActiveFocus()
                }
                return
            }
            captureController.pointerPress(screenIndex, mouse.x, mouse.y)
        }
        onReleased: mouse => {
            const wasMarker = captureController.tool === "marker"
            const before = captureController.annotations.length
            captureController.pointerRelease(screenIndex, mouse.x, mouse.y)
            lastMarkerClickAdded = wasMarker && captureController.annotations.length > before
        }
        onDoubleClicked: mouse => {
            if (mouse.button === Qt.LeftButton)
                root.copySelectionAt(mouse.x, mouse.y)
        }
    }

    Item {
        id: editor
        objectName: "textEditor"
        visible: false
        readonly property real inset: 6
        x: Math.max(0, Math.min(root.textX - inset, root.width - width))
        y: Math.max(0, Math.min(root.textY - root.textLineHeight / 2 - inset,
                                 root.height - height))
        z: 5
        width: Math.max(120, textInput.contentWidth + inset * 2)
        height: Math.max(1, textInput.lineCount) * root.textLineHeight + inset * 2
        function commitText() {
            if (!visible) return
            let value = textInput.text
            let textStartX = screenRect.x + x + inset
            let textStartY = screenRect.y + y + inset + root.textLineHeight / 2
            visible = false
            if (value.trim().length > 0)
                captureController.addText(textStartX, textStartY, value)
            root.forceActiveFocus()
        }
        function handleReturn(event) {
            if (event.modifiers & Qt.ShiftModifier) {
                textInput.insert(textInput.cursorPosition, "\n")
            } else {
                commitText()
            }
            event.accepted = true
        }

        Canvas {
            anchors.fill: parent
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            onPaint: {
                let context = getContext("2d")
                context.clearRect(0, 0, width, height)
                context.strokeStyle = captureController.annotationColor
                context.lineWidth = 1
                context.setLineDash([5, 4])
                context.strokeRect(0.5, 0.5, width - 1, height - 1)
            }
        }

        TextEdit {
            id: textInput
            objectName: "textEditorInput"
            x: editor.inset
            y: editor.inset
            width: editor.width - editor.inset * 2
            height: editor.height - editor.inset * 2
            color: captureController.annotationColor
            font.bold: true
            font.pixelSize: 22
            textFormat: TextEdit.PlainText
            wrapMode: TextEdit.NoWrap
            verticalAlignment: TextEdit.AlignVCenter
            selectByMouse: true
            Keys.onReturnPressed: event => editor.handleReturn(event)
            Keys.onEnterPressed: event => editor.handleReturn(event)
            Keys.onPressed: event => {
                if (event.key === Qt.Key_Alt &&
                    (event.modifiers === Qt.NoModifier || event.modifiers === Qt.AltModifier)) {
                    root.leaveTextTool()
                    event.accepted = true
                }
            }
            onActiveFocusChanged: {
                if (!activeFocus && editor.visible) editor.commitText()
            }
            Keys.onEscapePressed: event => {
                captureController.cancel()
                event.accepted = true
            }
        }

        MouseArea {
            anchors.fill: parent
            visible: editor.visible && textInput.text.length === 0
            acceptedButtons: Qt.LeftButton
            onDoubleClicked: mouse => {
                if (mouse.button === Qt.LeftButton)
                    root.copySelectionAt(editor.x + mouse.x, editor.y + mouse.y)
            }
        }
    }

    Rectangle {
        objectName: "selectionDimensions"
        visible: root.activeRect.width > 0 && root.activeRect.height > 0
        x: Math.max(8, Math.min(root.localX, root.width - width - 8))
        y: Math.max(8, Math.min(root.localY - height - 5, root.height - height - 8))
        width: dimensions.contentWidth + 18
        height: 25
        radius: 4
        color: "#d9272d35"
        Text {
            id: dimensions
            anchors.centerIn: parent
            color: "white"
            font.pixelSize: 12
            text: Math.round(root.activeRect.width) + " × " + Math.round(root.activeRect.height)
        }
    }

    AnnotationToolbar {
        id: annotationToolbar
        objectName: "annotationToolbar"
        editing: editor.visible
        editorRect: Qt.rect(editor.x, editor.y, editor.width, editor.height)
        anchors.fill: parent
        z: 10
        toolbarVisible: captureController.selected && !captureController.ocrVisible &&
                        captureController.toolbarScreen === root.screenIndex
        selectionRect: Qt.rect(root.localX, root.localY,
                               captureController.selection.width, captureController.selection.height)
        onCommitRequested: if (editor.visible) editor.commitText()
        onFocusRequested: root.forceActiveFocus()
    }

    Rectangle {
        id: ocrPanel
        objectName: "ocrPanel"
        visible: captureController.ocrVisible &&
                 captureController.toolbarScreen === root.screenIndex
        onVisibleChanged: if (visible) ocrText.forceActiveFocus()
        z: 15
        readonly property bool dark: captureController.darkToolbar
        readonly property color mutedInk: dark ? "#9aa7b4" : "#6b7580"
        readonly property color resultSurface: dark ? "#19212b" : "#f3f5f8"
        readonly property color previewSurface: "#111114"
        readonly property color accentFill: "#ff9f0a"
        readonly property color accentInk: "#17110a"
        readonly property real spaceBelow:
            root.height - (root.localY + captureController.selection.height) - 8
        readonly property real spaceAbove: root.localY - 8
        readonly property string previewSource:
            screenIndex >= 0 && captureController.imagesReady &&
            captureController.ocrVisible
            ? "image://captures/selection/" + captureController.captureGeneration + "/" +
              Math.round(captureController.selection.x) + "-" +
              Math.round(captureController.selection.y) + "-" +
              Math.round(captureController.selection.width) + "-" +
              Math.round(captureController.selection.height)
            : ""
        width: Math.min(856, root.width - 24)
        height: Math.min(366, root.height - 16)
        radius: 8
        color: annotationToolbar.panelSurface
        border.color: annotationToolbar.panelBorder
        clip: true
        x: Math.max(8, Math.min(root.width - width - 8,
             root.localX + captureController.selection.width / 2 - width / 2))
        y: Math.max(8, Math.min(root.height - height - 8,
             spaceBelow >= spaceAbove
             ? root.localY + captureController.selection.height + 8
             : root.localY - height - 8))

        Rectangle {
            id: ocrHeader
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 48
            color: "transparent"

            Rectangle {
                id: ocrTitleMark
                anchors.left: parent.left
                anchors.leftMargin: 15
                anchors.verticalCenter: parent.verticalCenter
                width: 26
                height: 26
                radius: 6
                color: Qt.rgba(1, 0.623, 0.039, 0.14)

                ToolbarGlyph {
                    anchors.centerIn: parent
                    width: 16
                    height: 16
                    action: "ocr"
                    accentColor: ocrPanel.accentFill
                    darkMode: ocrPanel.dark
                }
            }

            Text {
                id: ocrTitle
                objectName: "ocrTitle"
                anchors.left: ocrTitleMark.right
                anchors.leftMargin: 9
                anchors.verticalCenter: parent.verticalCenter
                text: "OCR"
                color: annotationToolbar.toolbarInk
                font.pixelSize: 14
                font.bold: true
            }

            Rectangle {
                id: ocrClose
                objectName: "ocrClose"
                anchors.right: parent.right
                anchors.rightMargin: 11
                anchors.verticalCenter: parent.verticalCenter
                width: 28
                height: 28
                radius: 5
                color: ocrCloseMouse.containsMouse ? annotationToolbar.toolbarHover
                                                   : "transparent"
                Accessible.name: qsTr("Close OCR result")
                Text {
                    anchors.centerIn: parent
                    text: "×"
                    color: annotationToolbar.toolbarInk
                    font.pixelSize: 18
                }
                MouseArea {
                    id: ocrCloseMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        captureController.closeOcr()
                        root.forceActiveFocus()
                    }
                }
            }
        }

        Item {
            id: ocrBody
            anchors.top: parent.top
            anchors.topMargin: ocrHeader.height
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: ocrFooter.top
            clip: true
            readonly property real previewWidth:
                Math.min(278, Math.max(150, width * 0.33))

            Rectangle {
                id: ocrPreviewPane
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                width: parent.previewWidth
                color: "transparent"

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 12
                    radius: 6
                    color: ocrPanel.previewSurface
                    clip: true

                    Image {
                        id: ocrPreviewImage
                        objectName: "ocrPreviewImage"
                        anchors.fill: parent
                        source: ocrPanel.previewSource
                        sourceSize.width: 560
                        sourceSize.height: 480
                        fillMode: Image.PreserveAspectFit
                        smooth: true
                        cache: false
                    }
                }
            }

            Rectangle {
                id: ocrDivider
                anchors.left: ocrPreviewPane.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.topMargin: 12
                anchors.bottomMargin: 12
                width: 1
                color: annotationToolbar.panelBorder
                opacity: 0.55
            }

            Rectangle {
                id: ocrResultPane
                anchors.left: ocrDivider.right
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.margins: 12
                radius: 6
                color: ocrPanel.resultSurface
                clip: true

                Flickable {
                    id: ocrTextScroll
                    anchors.fill: parent
                    anchors.margins: 13
                    visible: !captureController.ocrRunning &&
                             captureController.ocrError.length === 0
                    clip: true
                    contentWidth: width
                    contentHeight: ocrText.contentHeight
                    interactive: false
                    boundsBehavior: Flickable.StopAtBounds
                    WheelHandler {
                        onWheel: event => {
                            const limit = Math.max(0, ocrTextScroll.contentHeight -
                                                   ocrTextScroll.height)
                            ocrTextScroll.contentY = Math.max(0, Math.min(
                                limit, ocrTextScroll.contentY - event.angleDelta.y / 2))
                        }
                    }
                    TextEdit {
                        id: ocrText
                        objectName: "ocrText"
                        width: ocrTextScroll.width
                        height: Math.max(ocrTextScroll.height, contentHeight)
                        text: captureController.ocrText
                        readOnly: true
                        selectByMouse: true
                        wrapMode: TextEdit.Wrap
                        color: annotationToolbar.toolbarInk
                        selectionColor: "#2f6feb"
                        selectedTextColor: "white"
                        font.pixelSize: 13
                        activeFocusOnPress: true
                        Keys.onEscapePressed: {
                            captureController.closeOcr()
                            root.forceActiveFocus()
                        }
                    }
                }

                Text {
                    anchors.fill: parent
                    anchors.margins: 16
                    visible: !captureController.ocrRunning &&
                             captureController.ocrError.length > 0
                    text: captureController.ocrError
                    color: ocrPanel.dark ? "#ff8a8a" : "#b42318"
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                    verticalAlignment: Text.AlignVCenter
                }

                Column {
                    id: ocrSkeleton
                    anchors.fill: parent
                    anchors.margins: 16
                    visible: captureController.ocrRunning
                    spacing: 11
                    Repeater {
                        model: [0.93, 0.87, 0.70, 0.54]
                        Rectangle {
                            width: ocrSkeleton.width * modelData
                            height: 8
                            radius: 4
                            color: annotationToolbar.toolbarInk
                            opacity: 0.12
                        }
                    }
                    SequentialAnimation on opacity {
                        running: captureController.ocrRunning
                        loops: Animation.Infinite
                        NumberAnimation { to: 0.45; duration: 700; easing.type: Easing.InOutSine }
                        NumberAnimation { to: 1; duration: 700; easing.type: Easing.InOutSine }
                    }
                }
            }
        }

        Rectangle {
            id: ocrFooter
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 52
            color: "transparent"

            Text {
                id: ocrStatus
                anchors.left: parent.left
                anchors.leftMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                visible: captureController.ocrRunning
                text: qsTr("Recognizing text…")
                color: ocrPanel.mutedInk
                font.pixelSize: 12
            }

            Rectangle {
                id: ocrCopy
                objectName: "ocrCopy"
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                width: Math.max(96, ocrCopyLabel.implicitWidth + 28)
                height: 32
                radius: 5
                visible: !captureController.ocrRunning &&
                         captureController.ocrError.length === 0 &&
                         captureController.ocrText.length > 0
                color: ocrCopyMouse.containsMouse ? Qt.lighter(ocrPanel.accentFill, 1.08)
                                                  : ocrPanel.accentFill
                Text {
                    id: ocrCopyLabel
                    anchors.centerIn: parent
                    text: captureController.ocrCopied ? qsTr("Copied") : qsTr("Copy")
                    color: ocrPanel.accentInk
                    font.pixelSize: 12
                    font.bold: true
                }
                MouseArea {
                    id: ocrCopyMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: captureController.copyOcrText()
                }
            }
        }
    }

    Rectangle {
        objectName: "statusPanel"
        visible: captureController.status.length > 0
        z: 20
        anchors.horizontalCenter: parent.horizontalCenter
        y: 8
        width: Math.min(statusText.implicitWidth + 24, root.width - 16)
        height: statusText.implicitHeight + 16
        radius: 5
        color: "#db9d2735"
        Text {
            id: statusText
            objectName: "statusText"
            anchors.centerIn: parent
            width: parent.width - 24
            text: captureController.status
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            horizontalAlignment: Qt.application.layoutDirection === Qt.RightToLeft
                                 ? Text.AlignRight : Text.AlignLeft
            color: "white"
        }
    }

    Connections {
        target: captureController
        property bool spotlightWasDrafted: false
        function onSelectionChanged() { marks.refresh() }
        function onAnnotationsChanged() { marks.refresh() }
        function onSelectedChanged() { marks.refresh() }
        function onDraftChanged() {
            if (draftMarks.visible) draftMarks.requestPaint()
            if (marks.spotlightDraft || spotlightWasDrafted) marks.refresh()
            spotlightWasDrafted = marks.spotlightDraft
        }
    }

    Item {
        objectName: "scrollWindowChoice"
        anchors.fill: parent
        z: 50
        visible: captureController.scrollState === 1
        Rectangle {
            anchors.fill: parent
            color: "#770a1420"
            MouseArea {
                anchors.fill: parent
                onClicked: captureController.cancelScrollChoice()
            }
        }
        Repeater {
            model: captureController.scrollCandidates
            delegate: Rectangle {
                id: scrollWindow
                required property var modelData
                objectName: "scrollWindowCandidate"
                x: modelData.x - root.screenRect.x
                y: modelData.y - root.screenRect.y
                width: modelData.width
                height: modelData.height
                color: windowChoiceMouse.containsMouse ? "#557cc6f5" : "#337cc6f5"
                border.color: windowChoiceMouse.containsMouse ? "#ffffff" : "#a9dcff"
                border.width: 2
                Rectangle {
                    anchors.centerIn: parent
                    width: Math.min(scrollWindow.width - 16,
                                    Math.max(windowTitle.visible ? windowTitle.implicitWidth : 0,
                                             windowAction.implicitWidth) + 24)
                    height: windowTitle.visible ? 58 : 34
                    radius: 6
                    color: annotationToolbar.toolbarSurface
                    border.color: annotationToolbar.panelBorder
                    Column {
                        anchors.centerIn: parent
                        width: parent.width - 16
                        spacing: 4
                        Text {
                            id: windowTitle
                            visible: scrollWindow.width >= 160 && scrollWindow.height >= 76
                            width: parent.width
                            text: scrollWindow.modelData.title || qsTr("Window %1").arg(scrollWindow.modelData.index + 1)
                            horizontalAlignment: Text.AlignHCenter
                            elide: Text.ElideRight
                            color: annotationToolbar.toolbarInk
                            font.pixelSize: 12
                        }
                        Text {
                            id: windowAction
                            width: parent.width
                            text: scrollWindow.width >= 160 ? qsTr("Click to start scrolling capture") : qsTr("Click")
                            horizontalAlignment: Text.AlignHCenter
                            color: annotationToolbar.toolbarInk
                            font.pixelSize: 13
                            font.bold: true
                        }
                    }
                }
                MouseArea {
                    id: windowChoiceMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: captureController.chooseScrollWindow(modelData.index)
                }
            }
        }
        Rectangle {
            objectName: "scrollWindowChoiceHint"
            width: Math.min(root.width - 24,
                            Math.max(hintTitle.implicitWidth, hintDetail.implicitWidth) + 32)
            height: 64
            x: Math.max(12, Math.min(root.localX + captureController.selection.width / 2 - width / 2,
                                    root.width - width - 12))
            y: root.localY - height - 12 >= 12 ? root.localY - height - 12 :
               root.localY + captureController.selection.height + height + 24 <= root.height
               ? root.localY + captureController.selection.height + 12 : 12
            radius: 6
            color: annotationToolbar.toolbarSurface
            border.color: annotationToolbar.panelBorder
            Column {
                anchors.centerIn: parent
                spacing: 5
                Text {
                    id: hintTitle
                    text: qsTr("Click a window to start scrolling capture")
                    color: annotationToolbar.toolbarInk
                    font.pixelSize: 15
                    font.bold: true
                }
                Text {
                    id: hintDetail
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("The selection contains several windows · Esc to cancel")
                    color: annotationToolbar.toolbarInk
                    font.pixelSize: 12
                }
            }
        }
    }
}
