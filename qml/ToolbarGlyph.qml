// SPDX-FileCopyrightText: 2026 Andy Stewart
// SPDX-License-Identifier: GPL-3.0-only

import QtQuick

Canvas {
    id: glyph
    required property string action
    property color accentColor: "#ff4b55"
    property bool darkMode: false
    width: 22
    height: 22
    antialiasing: true
    onActionChanged: requestPaint()
    onAccentColorChanged: requestPaint()
    onDarkModeChanged: requestPaint()

    onPaint: {
        const ctx = getContext("2d")
        const ink = darkMode ? "#e9f0f6" : "#293340"
        const paper = darkMode ? "#222b36" : "#ffffff"
        const brightness = accentColor.r * 0.2126 +
                           accentColor.g * 0.7152 + accentColor.b * 0.0722
        const iconFill = brightness > 0.87 ? "#d9e2ec" : accentColor
        ctx.clearRect(0, 0, width, height)
        ctx.save()
        ctx.scale(width / 24, height / 24)
        ctx.strokeStyle = ink
        ctx.fillStyle = paper
        ctx.lineWidth = 1.8
        ctx.lineCap = "round"
        ctx.lineJoin = "round"

        function strokePath(draw) {
            ctx.beginPath()
            draw()
            ctx.stroke()
        }

        function fillPath(draw, fill) {
            ctx.beginPath()
            draw()
            ctx.fillStyle = fill
            ctx.fill()
            ctx.strokeStyle = ink
            ctx.stroke()
        }

        function roundedRect(x, y, w, h, r) {
            ctx.moveTo(x + r, y)
            ctx.lineTo(x + w - r, y)
            ctx.quadraticCurveTo(x + w, y, x + w, y + r)
            ctx.lineTo(x + w, y + h - r)
            ctx.quadraticCurveTo(x + w, y + h, x + w - r, y + h)
            ctx.lineTo(x + r, y + h)
            ctx.quadraticCurveTo(x, y + h, x, y + h - r)
            ctx.lineTo(x, y + r)
            ctx.quadraticCurveTo(x, y, x + r, y)
            ctx.closePath()
        }

        function dot(x, y, radius, color) {
            ctx.beginPath()
            ctx.arc(x, y, radius, 0, Math.PI * 2)
            ctx.fillStyle = color
            ctx.fill()
        }

        function curvedShaft() {
            ctx.moveTo(4.2, 19)
            ctx.bezierCurveTo(3.9, 8.2, 10.1, 4.4, 19.2, 6.2)
        }

        function undoArrow() {
            ctx.moveTo(9.2, 5.5)
            ctx.lineTo(4.5, 10.2)
            ctx.lineTo(9.2, 14.8)
            ctx.moveTo(4.8, 10.2)
            ctx.lineTo(12.8, 10.2)
            ctx.bezierCurveTo(16.8, 10.2, 19.3, 12.7, 19.3, 18.2)
        }

        switch (action) {
        case "resume":
            strokePath(() => {
                ctx.moveTo(7, 4)
                ctx.lineTo(19, 12)
                ctx.lineTo(7, 20)
                ctx.closePath()
            })
            break
        case "zoom_in":
        case "zoom_out":
            strokePath(() => {
                ctx.arc(10, 10, 6.5, 0, Math.PI * 2)
                ctx.moveTo(15, 15); ctx.lineTo(21, 21)
                ctx.moveTo(6.5, 10); ctx.lineTo(13.5, 10)
                if (action === "zoom_in") {
                    ctx.moveTo(10, 6.5); ctx.lineTo(10, 13.5)
                }
            })
            break
        case "scroll":
            strokePath(() => {
                ctx.rect(5, 3, 14, 18)
                ctx.moveTo(12, 7); ctx.lineTo(12, 16)
                ctx.moveTo(8.5, 12.5); ctx.lineTo(12, 16); ctx.lineTo(15.5, 12.5)
            })
            break
        case "color":
            fillPath(() => {
                ctx.moveTo(11.8, 2.7)
                ctx.bezierCurveTo(6.6, 2.7, 2.8, 6.6, 2.8, 12)
                ctx.bezierCurveTo(2.8, 17.4, 6.8, 21.2, 12.2, 21.2)
                ctx.bezierCurveTo(14.4, 21.2, 15.8, 19.8, 15.8, 18)
                ctx.bezierCurveTo(15.8, 16.6, 14.4, 16, 14.4, 14.7)
                ctx.bezierCurveTo(14.4, 13.4, 15.3, 12.8, 16.4, 12.8)
                ctx.lineTo(18.1, 12.8)
                ctx.bezierCurveTo(20, 12.8, 21.2, 11.5, 20.9, 9.7)
                ctx.bezierCurveTo(20.3, 5.6, 16.5, 2.7, 11.8, 2.7)
                ctx.closePath()
            }, paper)
            dot(7.2, 10, 2, accentColor)
            ctx.lineWidth = 0.7
            ctx.stroke()
            ctx.lineWidth = 1.8
            dot(10.5, 6.4, 1.35, "#60b6e9")
            dot(15.7, 7, 1.35, "#f3bd61")
            dot(8.6, 16.1, 1.35, "#8fbf88")
            break

        case "select":
            fillPath(() => {
                ctx.moveTo(3.3, 2.7)
                ctx.lineTo(3.3, 20.1)
                ctx.lineTo(8.1, 15.9)
                ctx.lineTo(11.1, 21.3)
                ctx.lineTo(13.8, 19.8)
                ctx.lineTo(10.8, 14.4)
                ctx.lineTo(17.4, 14.2)
                ctx.closePath()
            }, paper)
            break

        case "rect":
            strokePath(() => ctx.rect(4, 4, 16, 16))
            break
        case "roundrect":
            strokePath(() => roundedRect(4, 4, 16, 16, 4))
            break
        case "fillrect":
            fillPath(() => ctx.rect(4, 4, 16, 16), iconFill)
            break
        case "ellipse":
            strokePath(() => ctx.arc(12, 12, 8.1, 0, Math.PI * 2))
            break
        case "fillellipse":
            fillPath(() => ctx.arc(12, 12, 8.1, 0, Math.PI * 2), iconFill)
            break
        case "spotlight":
            ctx.fillStyle = "#c4ced9"
            ctx.fillRect(3.5, 3.5, 17, 17)
            strokePath(() => ctx.rect(3.5, 3.5, 17, 17))
            fillPath(() => ctx.arc(12, 12, 6.2, 0, Math.PI * 2), paper)
            break

        case "arrow":
            strokePath(() => {
                ctx.moveTo(4.2, 19.8)
                ctx.lineTo(19.6, 4.4)
                ctx.moveTo(11.4, 4.4)
                ctx.lineTo(19.6, 4.4)
                ctx.lineTo(19.6, 12.6)
            })
            break
        case "curvedarrow":
        case "doublearrow":
            strokePath(() => {
                curvedShaft()
                ctx.moveTo(13, 3.2)
                ctx.lineTo(19.2, 6.2)
                ctx.lineTo(15.4, 11.8)
                if (action === "doublearrow") {
                    ctx.moveTo(4.2, 19)
                    ctx.lineTo(3.8, 12.3)
                    ctx.moveTo(4.2, 19)
                    ctx.lineTo(10.7, 18)
                }
            })
            break
        case "line":
            strokePath(() => {
                ctx.moveTo(4.5, 19.5)
                ctx.lineTo(19.5, 4.5)
            })
            dot(4.5, 19.5, 1.25, ink)
            dot(19.5, 4.5, 1.25, ink)
            break

        case "pen":
            fillPath(() => {
                ctx.moveTo(4.2, 20.1)
                ctx.lineTo(5.2, 15.4)
                ctx.lineTo(15.8, 4.8)
                ctx.quadraticCurveTo(16.9, 3.7, 18, 4.8)
                ctx.lineTo(19.2, 6)
                ctx.quadraticCurveTo(20.3, 7.1, 19.2, 8.2)
                ctx.lineTo(8.6, 18.8)
                ctx.closePath()
            }, paper)
            strokePath(() => {
                ctx.moveTo(5.2, 15.4)
                ctx.lineTo(8.6, 18.8)
                ctx.moveTo(14.8, 5.8)
                ctx.lineTo(18.2, 9.2)
            })
            break
        case "highlighter":
            ctx.save()
            ctx.globalAlpha = 0.38
            ctx.strokeStyle = iconFill
            ctx.lineWidth = 5.5
            strokePath(() => {
                ctx.moveTo(3.6, 20)
                ctx.lineTo(10.1, 13.5)
            })
            ctx.restore()
            fillPath(() => {
                ctx.moveTo(8.1, 15.2)
                ctx.lineTo(15.8, 4.9)
                ctx.quadraticCurveTo(16.6, 3.9, 17.6, 4.9)
                ctx.lineTo(19.2, 6.5)
                ctx.quadraticCurveTo(20.2, 7.5, 19.2, 8.3)
                ctx.lineTo(9, 16.1)
                ctx.closePath()
            }, iconFill)
            strokePath(() => {
                ctx.moveTo(8.1, 15.2)
                ctx.lineTo(5.7, 18.5)
                ctx.lineTo(8.4, 21.2)
                ctx.lineTo(11.7, 18.8)
            })
            break
        case "text":
            strokePath(() => {
                ctx.moveTo(4.4, 20)
                ctx.lineTo(12, 4)
                ctx.lineTo(19.6, 20)
                ctx.moveTo(7.5, 14.7)
                ctx.lineTo(16.5, 14.7)
            })
            break
        case "mosaic":
            ctx.fillStyle = "#202020"
            ctx.fillRect(4, 4, 8, 8)
            ctx.fillStyle = "#ededed"
            ctx.fillRect(12, 4, 8, 8)
            ctx.fillStyle = "#a4a4a4"
            ctx.fillRect(4, 12, 8, 8)
            ctx.fillStyle = "#555555"
            ctx.fillRect(12, 12, 8, 8)
            ctx.strokeStyle = darkMode ? "#f4f4f4" : "#242424"
            strokePath(() => {
                ctx.rect(4, 4, 16, 16)
                ctx.moveTo(12, 4)
                ctx.lineTo(12, 20)
                ctx.moveTo(4, 12)
                ctx.lineTo(20, 12)
            })
            break
        case "marker": {
            strokePath(() => ctx.arc(12, 12, 8.5, 0, Math.PI * 2))
            ctx.lineWidth = 2.1
            strokePath(() => {
                ctx.moveTo(9.8, 10.1)
                ctx.lineTo(12.3, 8.1)
                ctx.lineTo(12.3, 16.5)
                ctx.moveTo(9.6, 16.6)
                ctx.lineTo(15, 16.6)
            })
            break
        }

        case "theme_light":
            strokePath(() => ctx.arc(12, 12, 4.2, 0, Math.PI * 2))
            for (let i = 0; i < 8; ++i) {
                const angle = i * Math.PI / 4
                const dx = Math.cos(angle)
                const dy = Math.sin(angle)
                strokePath(() => {
                    ctx.moveTo(12 + dx * 7.2, 12 + dy * 7.2)
                    ctx.lineTo(12 + dx * 9.8, 12 + dy * 9.8)
                })
            }
            break
        case "theme_dark":
            strokePath(() => {
                ctx.arc(12, 12, 8.4, -Math.PI * .3, Math.PI * .7)
                ctx.bezierCurveTo(8.2, 17.8, 7.7, 8.4, 15.3, 4.1)
            })
            break

        case "undo":
            strokePath(undoArrow)
            break
        case "redo":
            ctx.save()
            ctx.translate(24, 0)
            ctx.scale(-1, 1)
            strokePath(undoArrow)
            ctx.restore()
            break
        case "pin":
            ctx.save()
            ctx.translate(12, 12)
            ctx.rotate(Math.PI / 4)
            ctx.translate(-12, -12)
            ctx.lineWidth = 1.7
            fillPath(() => {
                roundedRect(8, 3.5, 8, 2.7, 0.9)
            }, ink)
            fillPath(() => {
                ctx.moveTo(9.7, 6.2)
                ctx.lineTo(14.3, 6.2)
                ctx.lineTo(14.3, 9.8)
                ctx.quadraticCurveTo(14.3, 10.7, 15, 11.4)
                ctx.lineTo(16.5, 13)
                ctx.quadraticCurveTo(17.1, 13.6, 16.8, 14.3)
                ctx.quadraticCurveTo(16.6, 14.8, 15.9, 14.8)
                ctx.lineTo(8.1, 14.8)
                ctx.quadraticCurveTo(7.4, 14.8, 7.2, 14.3)
                ctx.quadraticCurveTo(6.9, 13.6, 7.5, 13)
                ctx.lineTo(9, 11.4)
                ctx.quadraticCurveTo(9.7, 10.7, 9.7, 9.8)
                ctx.closePath()
            }, paper)
            strokePath(() => {
                ctx.moveTo(12, 14.8)
                ctx.lineTo(12, 21)
            })
            ctx.restore()
            break
        case "copy":
            fillPath(() => roundedRect(3.3, 3.3, 12.7, 12.7, 1.5), paper)
            fillPath(() => roundedRect(8, 8, 12.7, 12.7, 1.5), paper)
            break
        case "ocr":
            strokePath(() => {
                ctx.moveTo(4, 9); ctx.lineTo(4, 4); ctx.lineTo(9, 4)
                ctx.moveTo(15, 4); ctx.lineTo(20, 4); ctx.lineTo(20, 9)
                ctx.moveTo(20, 15); ctx.lineTo(20, 20); ctx.lineTo(15, 20)
                ctx.moveTo(9, 20); ctx.lineTo(4, 20); ctx.lineTo(4, 15)
            })
            strokePath(() => {
                ctx.moveTo(8, 9); ctx.lineTo(16, 9)
                ctx.moveTo(8, 12); ctx.lineTo(16, 12)
                ctx.moveTo(8, 15); ctx.lineTo(13, 15)
            })
            break
        case "save":
            strokePath(() => {
                ctx.moveTo(12, 3.4)
                ctx.lineTo(12, 15)
                ctx.moveTo(7.4, 10.7)
                ctx.lineTo(12, 15.3)
                ctx.lineTo(16.6, 10.7)
                ctx.moveTo(4.5, 16.9)
                ctx.lineTo(4.5, 20)
                ctx.lineTo(19.5, 20)
                ctx.lineTo(19.5, 16.9)
            })
            break
        }
        ctx.restore()
    }
}
