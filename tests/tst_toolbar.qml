// SPDX-FileCopyrightText: 2026 Andy Stewart
// SPDX-License-Identifier: GPL-3.0-only

import QtQuick
import QtQuick.Window
import QtTest
import "../qml" as Screenshot

TestCase {
    id: testCase
    name: "SharedScreenshotToolbar"
    width: 1280
    height: 720
    when: windowShown
    visible: true
    Keys.onPressed: event => {
        if (longToolbar.visible) longToolbar.handleShortcut(event)
        else normalToolbar.handleShortcut(event)
    }

    QtObject {
        id: captureController
        property bool selected: true
        property bool darkToolbar: false
        property string annotationColor: "#ff4b55"
        property var annotations: []
        property var draft: ({})
        property string tool: "select"
        property var toolVariants: ({rect: "rect", ellipse: "ellipse", arrow: "arrow", pen: "pen"})
        property int startCount: 0
        property int resumeCount: 0
        property int pinCount: 0
        property int ocrCount: 0
        property rect scrollRegion: Qt.rect(40, 100, 1100, 300)
        property int scrollHeight: 0
        property bool scrollAwaitingPane: false
        property bool scrollPaneSelected: false
        property bool scrollStopping: false
        property int scrollState: 2
        signal scrollFrameAboutToCapture()
        signal scrollFrameCaptured()
        function activateToolGroup(group) { tool = toolVariants[group] }
        function saveAnnotationColor() {}
        function setAnnotationColorFromHsv(h, s, v) { annotationColor = "#ff9d42" }
        function startScroll() { startCount++ }
        function resumeScroll() { resumeCount++ }
        function pin() { pinCount++ }
        function startOcr() { ocrCount++ }
    }
    Screenshot.AnnotationToolbar {
        id: normalToolbar
        anchors.fill: parent
        selectionRect: Qt.rect(40, 100, 1100, 300)
    }
    Screenshot.AnnotationToolbar {
        id: longToolbar
        anchors.fill: parent
        longImage: true
        visible: false
    }
    Screenshot.ScrollCapture {
        id: scrollCapture
        anchors.fill: parent
        screenRect: Qt.rect(0, 0, 1280, 720)
        visible: false
    }
    TextEdit {
        id: annotationInput
        visible: false
        width: 300
        height: 40
    }
    SignalSpy { id: zoomSpy; target: longToolbar; signalName: "zoomRequested" }

    function visualItem(item, name) {
        if (item.objectName === name) return item
        for (let child of item.children) {
            let found = visualItem(child, name)
            if (found) return found
        }
        return null
    }
    function initTestCase() {
        Window.window.width = 1280
        Window.window.height = 720
        wait(30)
    }
    function init() {
        captureController.selected = true
        captureController.tool = "select"
        captureController.darkToolbar = false
        captureController.annotations = []
        captureController.startCount = 0
        captureController.resumeCount = 0
        captureController.pinCount = 0
        captureController.ocrCount = 0
        normalToolbar.visible = true
        longToolbar.visible = false
        scrollCapture.visible = false
        scrollCapture.frameHidden = false
        captureController.scrollHeight = 0
        normalToolbar.colorPanelVisible = false
        normalToolbar.variantPanelVisible = false
        longToolbar.colorPanelVisible = false
        longToolbar.variantPanelVisible = false
        annotationInput.visible = false
        annotationInput.text = ""
        zoomSpy.clear()
        testCase.forceActiveFocus()
        wait(20)
    }
    function test_scroll_and_zoom_key_icon_style() {
        let standard = visualItem(normalToolbar, "tool_select")
        let scroll = visualItem(normalToolbar, "tool_scroll")
        compare(scroll.width, standard.width)
        compare(visualItem(normalToolbar, "toolLabel_scroll").text, "H:")
        normalToolbar.visible = false
        longToolbar.visible = true
        for (let action of [{name: "resume", key: "H"},
                            {name: "zoom_in", key: "="},
                            {name: "zoom_out", key: "-"}]) {
            let button = visualItem(longToolbar, "longTool_" + action.name)
            let label = visualItem(longToolbar, "longLabel_" + action.name)
            let glyph = visualItem(longToolbar, "longGlyph_" + action.name)
            compare(button.width, standard.width)
            compare(button.height, standard.height)
            compare(label.text, action.key + ":")
            verify(label.visible)
            verify(glyph !== null)
        }
    }
    function test_ocr_button_and_shortcut() {
        let button = visualItem(normalToolbar, "tool_ocr")
        verify(button !== null)
        verify(button.visible)
        compare(visualItem(normalToolbar, "toolLabel_ocr").text, "O:")
        verify(visualItem(normalToolbar, "toolGlyph_ocr") !== null)
        keyClick(Qt.Key_O)
        compare(captureController.ocrCount, 1)
        captureController.annotations = [{type: "marker"}]
        keyClick(Qt.Key_O)
        compare(captureController.ocrCount, 2)
        normalToolbar.visible = false
        longToolbar.visible = true
        keyClick(Qt.Key_O)
        compare(captureController.ocrCount, 2)
    }
    function test_pin_button_and_shortcut_data() {
        return [{tag: "normal", longImage: false}, {tag: "long", longImage: true}]
    }
    function test_pin_button_and_shortcut(data) {
        normalToolbar.visible = !data.longImage
        longToolbar.visible = data.longImage
        const toolbar = data.longImage ? longToolbar : normalToolbar
        const prefix = data.longImage ? "longTool_" : "tool_"
        const button = visualItem(toolbar, prefix + "pin")
        verify(button !== null)
        const copy = visualItem(toolbar, prefix + "copy")
        verify(button.x + button.width <= copy.x)
        mouseClick(button, button.width / 2, button.height / 2)
        compare(captureController.pinCount, 1)
        testCase.forceActiveFocus()
        keyClick(Qt.Key_P)
        compare(captureController.pinCount, 2)
        keyClick(Qt.Key_P, Qt.ControlModifier)
        compare(captureController.pinCount, 2)
        captureController.selected = false
        keyClick(Qt.Key_P)
        compare(captureController.pinCount, 2)
        annotationInput.visible = true
        annotationInput.forceActiveFocus()
        keyClick(Qt.Key_P)
        compare(annotationInput.text, "p")
        compare(captureController.pinCount, 2)
    }
    function test_h_starts_only_before_annotations() {
        keyClick(Qt.Key_H)
        compare(captureController.startCount, 1)
        compare(captureController.resumeCount, 0)
        keyClick(Qt.Key_H, Qt.ControlModifier)
        keyClick(Qt.Key_H, Qt.ShiftModifier)
        compare(captureController.startCount, 1)
        captureController.annotations = [{type: "marker"}]
        keyClick(Qt.Key_H)
        compare(captureController.startCount, 1)
        captureController.annotations = []
        captureController.selected = false
        keyClick(Qt.Key_H)
        compare(captureController.startCount, 1)
    }
    function test_long_image_shortcuts_and_text_input() {
        normalToolbar.visible = false
        longToolbar.visible = true
        captureController.annotations = [{type: "marker"}]
        keyClick(Qt.Key_H)
        compare(captureController.resumeCount, 1)
        compare(captureController.startCount, 0)
        compare(captureController.annotations.length, 1)
        keyClick(Qt.Key_Equal)
        keyClick(Qt.Key_Minus)
        compare(zoomSpy.count, 2)
        compare(zoomSpy.signalArguments[0][0], 1.25)
        compare(zoomSpy.signalArguments[1][0], 0.8)
        keyClick(Qt.Key_H, Qt.ControlModifier)
        keyClick(Qt.Key_Equal, Qt.ControlModifier)
        keyClick(Qt.Key_Minus, Qt.ControlModifier)
        compare(captureController.resumeCount, 1)
        compare(zoomSpy.count, 2)
        annotationInput.visible = true
        annotationInput.forceActiveFocus()
        keyClick(Qt.Key_H)
        keyClick(Qt.Key_Equal)
        keyClick(Qt.Key_Minus)
        compare(annotationInput.text, "h=-")
        compare(captureController.resumeCount, 1)
        compare(zoomSpy.count, 2)
    }
    function test_scroll_after_marker() {
        let marker = visualItem(normalToolbar, "tool_marker")
        let scroll = visualItem(normalToolbar, "tool_scroll")
        let undo = visualItem(normalToolbar, "tool_undo")
        verify(marker !== null && scroll !== null && undo !== null)
        compare(marker.y, scroll.y)
        compare(scroll.y, undo.y)
        verify(marker.x + marker.width <= scroll.x)
        verify(scroll.x + scroll.width <= undo.x)
        captureController.annotations = [{type: "marker"}]
        tryCompare(scroll, "visible", false)
    }
    function test_group_style_and_palette() {
        let normalSelect = visualItem(normalToolbar, "tool_select")
        let longSelect = visualItem(longToolbar, "longTool_select")
        compare(normalSelect.width, longSelect.width)
        compare(normalSelect.height, longSelect.height)
        let normalGlyph = visualItem(normalToolbar, "selectCursorGlyph")
        let longGlyph = visualItem(longToolbar, "longGlyph_select")
        compare(normalGlyph.width, longGlyph.width)
        compare(normalGlyph.height, longGlyph.height)
        for (let dark of [false, true]) {
            captureController.darkToolbar = dark
            let normalPanel = visualItem(normalToolbar, "toolbar")
            let longPanel = visualItem(longToolbar, "longToolbar")
            compare(normalPanel.color, longPanel.color)
            compare(normalPanel.border.color, longPanel.border.color)
            compare(normalPanel.radius, longPanel.radius)
            compare(visualItem(normalToolbar, "selectHighlight").color,
                    visualItem(longToolbar, "longHighlight_select").color)
        }
        normalToolbar.visible = false
        longToolbar.visible = true
        verify(!longToolbar.toolbarHasAction("roundrect"))
        verify(!longToolbar.toolbarHasAction("spotlight"))
        verify(!longToolbar.toolbarHasAction("scroll"))
        let group = visualItem(longToolbar, "longTool_rect_group")
        mouseClick(group, group.width / 2, group.height / 2)
        tryCompare(longToolbar, "variantPanelVisible", true)
        wait(20)
        let option = longToolbar.variantOptionCenter(1)
        mouseClick(longToolbar, option.x, option.y)
        compare(captureController.tool, "roundrect")
        compare(longToolbar.variantPanelVisible, false)
        let color = visualItem(longToolbar, "colorButton")
        mouseClick(color, color.width / 2, color.height / 2)
        tryCompare(longToolbar, "colorPanelVisible", true)
        let swatch = longToolbar.colorPresetCenter(1)
        mouseClick(longToolbar, swatch.x, swatch.y)
        compare(captureController.annotationColor, "#ff9d42")
        compare(longToolbar.colorPanelVisible, false)
    }
    // A resident daemon reuses the toolbar, so nothing may stay open into the
    // next capture.
    function test_close_popups_for_next_capture() {
        let color = visualItem(normalToolbar, "colorButton")
        mouseClick(color, color.width / 2, color.height / 2)
        tryCompare(normalToolbar, "colorPanelVisible", true)
        normalToolbar.closePopups()
        compare(normalToolbar.colorPanelVisible, false)

        let group = visualItem(normalToolbar, "tool_rect_group")
        mouseClick(group, group.width / 2, group.height / 2)
        tryCompare(normalToolbar, "variantPanelVisible", true)
        normalToolbar.closePopups()
        compare(normalToolbar.variantPanelVisible, false)

        normalToolbar.showToolbarTooltip("hint", color)
        normalToolbar.closePopups()
        wait(500)
        compare(normalToolbar.toolbarTooltipVisible, false)
    }
    function test_palette_allows_toolbar_shortcuts() {
        let normalColor = visualItem(normalToolbar, "colorButton")
        mouseClick(normalColor, normalColor.width / 2, normalColor.height / 2)
        tryCompare(normalToolbar, "colorPanelVisible", true)
        keyClick(Qt.Key_R)
        compare(captureController.tool, "rect")
        compare(normalToolbar.colorPanelVisible, false)

        keyClick(Qt.Key_Q)
        compare(normalToolbar.colorPanelVisible, true)
        keyClick(Qt.Key_Q)
        compare(normalToolbar.colorPanelVisible, false)

        normalToolbar.visible = false
        longToolbar.visible = true
        let longColor = visualItem(longToolbar, "colorButton")
        mouseClick(longColor, longColor.width / 2, longColor.height / 2)
        tryCompare(longToolbar, "colorPanelVisible", true)
        keyClick(Qt.Key_Equal)
        compare(zoomSpy.count, 1)
        compare(zoomSpy.signalArguments[0][0], 1.25)
        compare(longToolbar.colorPanelVisible, false)
    }
    function test_zoom_and_resume_before_undo() {
        normalToolbar.visible = false
        longToolbar.visible = true
        let zoomOut = visualItem(longToolbar, "longTool_zoom_out")
        let zoomIn = visualItem(longToolbar, "longTool_zoom_in")
        let resume = visualItem(longToolbar, "longTool_resume")
        let marker = visualItem(longToolbar, "longTool_marker")
        let undo = visualItem(longToolbar, "longTool_undo")
        verify(zoomOut !== null && zoomIn !== null && resume !== null &&
               marker !== null && undo !== null)
        compare(marker.y, zoomOut.y)
        compare(zoomOut.y, zoomIn.y)
        compare(zoomIn.y, resume.y)
        compare(resume.y, undo.y)
        verify(marker.x + marker.width <= zoomOut.x)
        verify(zoomOut.x + zoomOut.width <= zoomIn.x)
        verify(zoomIn.x + zoomIn.width <= resume.x)
        verify(resume.x + resume.width <= undo.x)
        let glyph = visualItem(longToolbar, "longGlyph_zoom_in")
        let selectGlyph = visualItem(longToolbar, "longGlyph_select")
        compare(glyph.width, selectGlyph.width)
        compare(glyph.height, selectGlyph.height)
        compare(visualItem(longToolbar, "longHighlight_zoom_in").color.a, 0)
        mouseClick(zoomOut, zoomOut.width / 2, zoomOut.height / 2)
        compare(zoomSpy.count, 1)
        compare(zoomSpy.signalArguments[0][0], 0.8)
        mouseClick(zoomIn, zoomIn.width / 2, zoomIn.height / 2)
        compare(zoomSpy.count, 2)
        compare(zoomSpy.signalArguments[1][0], 1.25)
        mouseClick(resume, resume.width / 2, resume.height / 2)
        compare(captureController.resumeCount, 1)
    }

    function test_scroll_hint_stays_visible_data() {
        return [
            {tag: "below", region: Qt.rect(40, 100, 1100, 300), outside: true},
            {tag: "above", region: Qt.rect(40, 100, 1100, 610), outside: true},
            {tag: "beside", region: Qt.rect(300, 0, 700, 720), outside: true},
            {tag: "full_screen", region: Qt.rect(0, 0, 1280, 720), outside: false}
        ]
    }
    function test_scroll_hint_stays_visible(data) {
        normalToolbar.visible = false
        scrollCapture.visible = true
        captureController.scrollRegion = data.region
        wait(20)
        let hint = visualItem(scrollCapture, "scrollToolbar")
        compare(scrollCapture.toolbarOutsideCapture, data.outside)
        compare(hint.visible, data.outside)
        captureController.scrollFrameAboutToCapture()
        compare(hint.visible, data.outside)
        captureController.scrollFrameCaptured()
        captureController.scrollHeight = data.region.height
        wait(20)
        verify(hint.visible)
        const x = hint.x
        const y = hint.y
        for (let frame = 0; frame < 4; ++frame) {
            captureController.scrollFrameAboutToCapture()
            verify(hint.visible)
            compare(hint.x, x)
            compare(hint.y, y)
            captureController.scrollFrameCaptured()
            verify(hint.visible)
        }
    }
}
