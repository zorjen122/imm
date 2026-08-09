import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import WimiClient

Rectangle {
    id: root

    signal sendRequested(string text)
    signal fileSelected(url fileUrl)
    signal draftEdited(string text)
    property alias draftText: input.text

    implicitHeight: Math.max(68, composerRow.implicitHeight + Tokens.space3 * 2)
    color: Theme.surface

    function submit() {
        const value = input.text.trim()
        if (value.length === 0)
            return
        root.sendRequested(value)
        input.clear()
    }

    RowLayout {
        id: composerRow
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: Math.max(Tokens.space3, (root.width - 900) / 2)
        anchors.rightMargin: Math.max(Tokens.space3, (root.width - 900) / 2)
        spacing: Tokens.space2

        ToolButton {
            text: "+"
            font.pixelSize: Typography.title
            enabled: true
            onClicked: fileDialog.open()
            Accessible.name: qsTr("添加附件")
            Accessible.description: qsTr("选择文件附件")
        }

        TextArea {
            id: input
            Layout.fillWidth: true
            Layout.minimumHeight: Tokens.touchTarget
            Layout.maximumHeight: 132
            placeholderText: qsTr("输入消息")
            wrapMode: TextEdit.Wrap
            selectByMouse: true
            Accessible.name: qsTr("消息输入框")
            onTextChanged: root.draftEdited(text)

            Keys.onReturnPressed: event => {
                if ((event.modifiers & Qt.ShiftModifier) !== 0 || input.inputMethodComposing) {
                    event.accepted = false
                    return
                }
                root.submit()
                event.accepted = true
            }

            background: Rectangle {
                radius: Tokens.radiusLarge
                color: Theme.surfaceMuted
                border.width: input.activeFocus ? 1 : 0
                border.color: Theme.accent
            }
        }

        Button {
            id: sendButton

            Layout.preferredWidth: Tokens.touchTarget
            Layout.preferredHeight: Tokens.touchTarget
            enabled: input.text.trim().length > 0
            onClicked: root.submit()
            Accessible.name: qsTr("发送消息")

            contentItem: AppIcon {
                source: Icons.send
                color: sendButton.enabled
                       ? (Theme.dark ? Theme.canvas : Theme.surface)
                       : Theme.textSecondary
            }

            background: Rectangle {
                radius: Tokens.radiusFull
                color: sendButton.enabled
                       ? (sendButton.down ? Theme.accentHover : Theme.accent)
                       : Theme.surfaceMuted
            }
        }
    }

    FileDialog {
        id: fileDialog
        title: qsTr("选择附件")
        onAccepted: root.fileSelected(selectedFile)
    }

    Rectangle {
        anchors.top: parent.top
        width: parent.width
        height: 1
        color: Theme.divider
    }
}
