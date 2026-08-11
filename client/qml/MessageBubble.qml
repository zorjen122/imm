import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import WimiClient

Item {
    id: root

    required property string messageText
    required property int messageType
    required property string fileId
    required property string fileName
    required property string fileType
    required property double fileSize
    required property bool fileExpired
    property url fileUrl
    property string markdownText: ""
    required property string timeText
    required property bool outgoing
    required property string deliveryState
    required property var clientMessageId
    property bool compactMode: false
    signal retryRequested(var clientMessageId)
    signal downloadRequested(string fileId, string fileType, double fileSize)

    readonly property string stateText: {
        switch (deliveryState) {
        case "pending": return qsTr("保存中")
        case "waiting-accept": return qsTr("发送中")
        case "accepted": return "✓"
        case "delivered": return "✓✓"
        case "read": return "✓✓"
        case "unknown": return qsTr("待确认")
        case "retryable-failed": return qsTr("可重试")
        case "permanent-failed": return qsTr("发送失败")
        default: return ""
        }
    }
    readonly property bool hasFileInfo: messageType === 5 && fileId.length > 0
                                        && fileName.length > 0 && fileType.length > 0 && fileSize >= 0
    readonly property bool hasImagePreview: hasFileInfo
                                             && (fileType === "image/png"
                                                 || fileType === "image/jpeg")
                                             && fileUrl.toString().length > 0
    readonly property color stateColor: {
        switch (deliveryState) {
        case "read": return Theme.accent
        case "unknown":
        case "retryable-failed": return Theme.warning
        case "permanent-failed": return Theme.error
        default: return Theme.textSecondary
        }
    }

    implicitHeight: bubble.implicitHeight + Tokens.space1

    function copyMessage() {
        messageTextItem.selectAll()
        messageTextItem.copy()
        messageTextItem.deselect()
    }

    function retryMessage() {
        root.retryRequested(root.clientMessageId)
    }

    HoverHandler {
        id: hoverHandler
    }

    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: messageMenu.popup()
    }

    TapHandler {
        acceptedButtons: Qt.LeftButton
        onLongPressed: {
            if (root.compactMode)
                messageActionSheet.open()
            else
                messageMenu.popup()
        }
    }

    Menu {
        id: messageMenu

        MenuItem {
            text: qsTr("复制")
            onTriggered: root.copyMessage()
        }

        MenuItem {
            text: qsTr("回复（待协议）")
            enabled: false
        }

        MenuItem {
            visible: root.deliveryState === "retryable-failed"
            text: qsTr("重试发送")
            onTriggered: root.retryMessage()
        }
    }

    Drawer {
        id: messageActionSheet

        parent: Overlay.overlay
        edge: Qt.BottomEdge
        width: parent ? parent.width : 0
        height: messageActions.implicitHeight + Tokens.space6
        modal: true
        dim: true

        contentItem: ColumnLayout {
            id: messageActions

            spacing: Tokens.space1

            Label {
                Layout.fillWidth: true
                Layout.leftMargin: Tokens.space4
                Layout.rightMargin: Tokens.space4
                Layout.topMargin: Tokens.space3
                text: qsTr("消息操作")
                color: Theme.textPrimary
                font.pixelSize: Typography.titleSmall
                font.bold: true
            }

            Button {
                Layout.fillWidth: true
                text: qsTr("复制")
                flat: true
                onClicked: {
                    root.copyMessage()
                    messageActionSheet.close()
                }
            }

            Button {
                Layout.fillWidth: true
                text: qsTr("回复（待协议）")
                flat: true
                enabled: false
            }

            Button {
                Layout.fillWidth: true
                visible: root.deliveryState === "retryable-failed"
                text: qsTr("重试发送")
                flat: true
                onClicked: {
                    root.retryMessage()
                    messageActionSheet.close()
                }
            }

            Button {
                Layout.fillWidth: true
                text: qsTr("取消")
                flat: true
                onClicked: messageActionSheet.close()
            }
        }
    }

    Rectangle {
        id: bubble

        anchors.right: root.outgoing ? parent.right : undefined
        anchors.left: root.outgoing ? undefined : parent.left
        width: Math.min(parent.width * 0.76, 540)
        implicitHeight: messageColumn.implicitHeight + Tokens.space3 * 2
        radius: Tokens.radiusLarge
        color: root.outgoing ? Theme.messageOutgoing : Theme.messageIncoming
        border.width: root.outgoing ? 0 : 1
        border.color: Theme.border

        ColumnLayout {
            id: messageColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Tokens.space3
            spacing: Tokens.space1

            TextEdit {
                id: messageTextItem

                Layout.fillWidth: true
                visible: root.messageType !== 5
                text: root.messageText
                color: Theme.textPrimary
                font.pixelSize: Typography.body
                wrapMode: Text.WrapAnywhere
                textFormat: Text.PlainText
                readOnly: true
                selectByMouse: true
                Accessible.name: root.messageText
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: root.messageType === 5
                spacing: Tokens.space1

                Label {
                    Layout.fillWidth: true
                    text: root.hasFileInfo ? root.fileName : qsTr("附件信息损坏")
                    color: Theme.textPrimary
                    font.pixelSize: Typography.body
                    elide: Text.ElideRight
                }
                Image {
                    Layout.fillWidth: true
                    Layout.maximumHeight: 280
                    visible: root.hasImagePreview
                    source: root.hasImagePreview ? root.fileUrl : ""
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                }
                Text {
                    Layout.fillWidth: true
                    visible: root.hasFileInfo && root.fileType === "text/markdown"
                             && root.markdownText.length > 0
                    text: root.markdownText
                    color: Theme.textPrimary
                    font.pixelSize: Typography.bodySmall
                    textFormat: Text.MarkdownText
                    wrapMode: Text.Wrap
                }
                Label {
                    visible: root.hasFileInfo
                    text: root.fileExpired
                          ? qsTr("文件已过期")
                          : qsTr("%1 · %2 字节").arg(root.fileType).arg(root.fileSize)
                    color: Theme.textSecondary
                    font.pixelSize: Typography.caption
                }
                Button {
                    visible: root.hasFileInfo && !root.fileExpired
                    text: qsTr("下载文件")
                    flat: true
                    onClicked: root.downloadRequested(root.fileId, root.fileType, root.fileSize)
                }
            }

            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: Tokens.space1

                Label {
                    text: root.timeText
                    color: Theme.textSecondary
                    font.pixelSize: Typography.caption
                }

                Label {
                    visible: root.outgoing
                    text: root.stateText
                    color: root.stateColor
                    font.pixelSize: Typography.caption
                    font.bold: root.deliveryState === "read"
                }
            }
        }
    }

    ToolButton {
        anchors.verticalCenter: bubble.verticalCenter
        anchors.right: root.outgoing ? bubble.left : undefined
        anchors.left: root.outgoing ? undefined : bubble.right
        visible: !root.compactMode
                 && (hoverHandler.hovered || messageMenu.opened)
        icon.source: Icons.more
        icon.color: Theme.textSecondary
        onClicked: messageMenu.popup()
        Accessible.name: qsTr("消息操作")
    }
}
