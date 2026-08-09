pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQml.Models
import WimiClient

Rectangle {
    id: root

    required property var controller
    property bool compactMode: false
    property bool searchVisible: false
    signal backRequested()

    color: Theme.canvas

    function setSearchVisible(visible) {
        if (searchVisible === visible)
            return
        if (visible)
            root.controller.saveTimelinePosition(timeline.contentY)
        searchVisible = visible
        if (visible) {
            Qt.callLater(messageSearchField.forceActiveFocus)
        } else {
            messageSearchField.clear()
            Qt.callLater(function() {
                timeline.contentY = root.controller.timelinePosition()
            })
        }
    }

    DelegateModel {
        id: filteredMessages
        model: root.controller.messages
        filterOnGroup: "visibleMessages"

        groups: DelegateModelGroup {
            name: "visibleMessages"
            includeByDefault: true
        }

        delegate: Item {
            id: messageDelegate

            required property var clientMessageId
            required property string body
            required property string timestamp
            required property int sourceIndex
            required property string dateLabel
            required property bool showDateSeparator
            required property bool showUnreadSeparator
            required property bool outgoing
            required property string deliveryState
            required property int messageType
            required property string fileId
            required property string fileName
            required property string fileType
            required property double fileSize
            required property bool fileExpired

            DelegateModel.groups: {
                const query = messageSearchField.text.trim().toLocaleLowerCase()
                return query.length === 0
                        || body.toLocaleLowerCase().includes(query)
                        ? ["visibleMessages"] : []
            }

            width: ListView.view.width
            height: messageColumn.implicitHeight

            Column {
                id: messageColumn

                width: parent.width
                spacing: Tokens.space1

                Item {
                    visible: messageDelegate.showDateSeparator
                    width: parent.width
                    height: visible ? 32 : 0

                    Rectangle {
                        anchors.centerIn: parent
                        width: dateLabel.implicitWidth + Tokens.space4
                        height: 26
                        radius: Tokens.radiusFull
                        color: Theme.surface
                        border.width: 1
                        border.color: Theme.border

                        Label {
                            id: dateLabel
                            anchors.centerIn: parent
                            text: messageDelegate.dateLabel
                            color: Theme.textSecondary
                            font.pixelSize: Typography.caption
                        }
                    }
                }

                Item {
                    visible: !root.searchVisible
                             && messageDelegate.showUnreadSeparator
                    width: parent.width
                    height: visible ? 32 : 0

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        height: 1
                        color: Theme.accent
                    }

                    Rectangle {
                        anchors.centerIn: parent
                        width: unreadLabel.implicitWidth + Tokens.space4
                        height: 24
                        color: Theme.canvas

                        Label {
                            id: unreadLabel
                            anchors.centerIn: parent
                            text: qsTr("未读消息")
                            color: Theme.accent
                            font.pixelSize: Typography.caption
                            font.bold: true
                        }
                    }
                }

                MessageBubble {
                    id: messageBubble

                    width: parent.width
                    messageText: messageDelegate.body
                    messageType: messageDelegate.messageType
                    fileId: messageDelegate.fileId
                    fileName: messageDelegate.fileName
                    fileType: messageDelegate.fileType
                    fileSize: messageDelegate.fileSize
                    fileExpired: messageDelegate.fileExpired
                    fileUrl: {
                        root.controller.filePreviewRevision
                        return root.controller.previewFileUrl(messageDelegate.fileId)
                    }
                    markdownText: root.controller.localMarkdownText(messageDelegate.fileId)
                    timeText: messageDelegate.timestamp
                    outgoing: messageDelegate.outgoing
                    deliveryState: messageDelegate.deliveryState
                    clientMessageId: messageDelegate.clientMessageId
                    compactMode: root.compactMode
                    onRetryRequested: clientMessageId =>
                        root.controller.retryMessage(clientMessageId)
                    onDownloadRequested: (fileId, fileType, fileSize) =>
                        root.controller.downloadFile(fileId, fileType, fileSize)
                }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 64
            color: Theme.surface

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Tokens.space3
                anchors.rightMargin: Tokens.space3
                spacing: Tokens.space2

                ToolButton {
                    visible: root.compactMode
                    icon.source: Icons.back
                    icon.color: Theme.textPrimary
                    onClicked: root.backRequested()
                    Accessible.name: qsTr("返回会话列表")
                }

                Avatar {
                    visible: root.controller.selectedConversationIndex >= 0
                    name: root.controller.selectedConversationTitle
                    size: Tokens.avatarMedium
                    avatarColor: Theme.accent
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0

                    Label {
                        Layout.fillWidth: true
                        text: root.controller.selectedConversationTitle
                        color: Theme.textPrimary
                        font.pixelSize: Typography.titleSmall
                        font.bold: true
                        elide: Text.ElideRight
                    }

                    Label {
                        visible: root.controller.selectedConversationIndex >= 0
                        text: root.controller.selectedConversationIsGroup
                              ? qsTr("群聊") : qsTr("最近在线")
                        color: Theme.textSecondary
                        font.pixelSize: Typography.caption
                    }
                }

                ToolButton {
                    icon.source: Icons.search
                    icon.color: Theme.textSecondary
                    enabled: root.controller.selectedConversationIndex >= 0
                    checked: root.searchVisible
                    onClicked: root.setSearchVisible(!root.searchVisible)
                    Accessible.name: qsTr("在会话中搜索")
                }

                ToolButton {
                    icon.source: Icons.more
                    icon.color: Theme.textSecondary
                    enabled: root.controller.selectedConversationIndex >= 0
                    onClicked: detailsDrawer.open()
                    Accessible.name: qsTr("会话详情")
                }
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: Theme.divider
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: root.searchVisible ? 56 : 0
            visible: root.searchVisible
            color: Theme.surface

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Math.max(Tokens.space3,
                                             (parent.width - 900) / 2)
                anchors.rightMargin: Math.max(Tokens.space3,
                                              (parent.width - 900) / 2)
                spacing: Tokens.space2

                TextField {
                    id: messageSearchField

                    Layout.fillWidth: true
                    placeholderText: qsTr("搜索当前会话")
                    selectByMouse: true
                    Accessible.name: qsTr("搜索当前会话消息")
                }

                Label {
                    visible: messageSearchField.text.length > 0
                    text: qsTr("%1 条结果").arg(timeline.count)
                    color: Theme.textSecondary
                    font.pixelSize: Typography.bodySmall
                }

                ToolButton {
                    text: "×"
                    onClicked: root.setSearchVisible(false)
                    Accessible.name: qsTr("关闭会话搜索")
                }
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: Theme.divider
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: timeline
                objectName: "timeline"
                anchors.fill: parent
                anchors.leftMargin: Math.max(Tokens.space3,
                                             (parent.width - 900) / 2)
                anchors.rightMargin: Math.max(Tokens.space3,
                                              (parent.width - 900) / 2)
                anchors.topMargin: Tokens.space3
                anchors.bottomMargin: Tokens.space2
                visible: root.controller.selectedConversationIndex >= 0
                clip: true
                spacing: Tokens.space1
                model: filteredMessages
                boundsBehavior: Flickable.StopAtBounds
                verticalLayoutDirection: ListView.TopToBottom

                ScrollBar.vertical: ScrollBar {}

                onCountChanged: {
                    if (!root.searchVisible)
                        Qt.callLater(positionViewAtEnd)
                }
                onMovementEnded: {
                    if (!root.searchVisible)
                        root.controller.saveTimelinePosition(contentY)
                }

                Component.onCompleted: Qt.callLater(function() {
                    contentY = root.controller.timelinePosition()
                })
                Component.onDestruction: {
                    if (!root.searchVisible)
                        root.controller.saveTimelinePosition(contentY)
                }
            }

            Button {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.rightMargin: Tokens.space4
                anchors.bottomMargin: Tokens.space3
                visible: !root.searchVisible
                         && root.controller.selectedConversationUnreadCount > 0
                text: qsTr("↓ %1 条未读")
                      .arg(root.controller.selectedConversationUnreadCount)
                onClicked: {
                    const firstUnread = Math.max(
                        0, timeline.count
                           - root.controller.selectedConversationUnreadCount)
                    timeline.positionViewAtIndex(firstUnread, ListView.Center)
                    root.controller.markRead(
                        root.controller.selectedConversationIndex)
                }
                Accessible.name: qsTr("跳到未读消息")
            }

            EmptyState {
                anchors.centerIn: parent
                visible: root.controller.selectedConversationIndex < 0
                title: qsTr("选择一个会话")
                description: qsTr("从会话列表选择联系人，或开始一个新会话。")
            }

            EmptyState {
                anchors.centerIn: parent
                visible: root.searchVisible
                         && messageSearchField.text.length > 0
                         && timeline.count === 0
                title: qsTr("没有匹配的消息")
                description: qsTr("请尝试其他关键词。")
                actionText: qsTr("清除搜索")
                onActionRequested: messageSearchField.clear()
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: Tokens.space3
            Layout.rightMargin: Tokens.space3
            Layout.topMargin: Tokens.space1
            visible: root.controller.serviceActionStatus.length > 0
            text: root.controller.serviceActionStatus
            color: Theme.textSecondary
            font.pixelSize: Typography.caption
            wrapMode: Text.Wrap
        }

        MessageComposer {
            id: composer

            Layout.fillWidth: true
            visible: root.controller.selectedConversationIndex >= 0
            onSendRequested: text => root.controller.sendMessage(text)
            onFileSelected: fileUrl => root.controller.sendFile(fileUrl)
            onDraftEdited: text => root.controller.draftText = text

            Component.onCompleted: draftText = root.controller.draftText
        }
    }

    Connections {
        target: root.controller

        function onSelectedConversationChanged() {
            composer.draftText = root.controller.draftText
            Qt.callLater(function() {
                timeline.contentY = root.controller.timelinePosition()
            })
        }

        function onDraftTextChanged() {
            if (composer.draftText !== root.controller.draftText)
                composer.draftText = root.controller.draftText
        }
    }

    Drawer {
        id: detailsDrawer

        edge: Qt.RightEdge
        width: Math.min(360, root.width * 0.9)
        height: root.height
        modal: root.compactMode
        dim: root.compactMode

        contentItem: Rectangle {
            color: Theme.surface

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Tokens.space4
                spacing: Tokens.space4

                RowLayout {
                    Layout.fillWidth: true

                    Label {
                        Layout.fillWidth: true
                        text: root.controller.selectedConversationIsGroup
                              ? qsTr("群聊详情") : qsTr("会话详情")
                        color: Theme.textPrimary
                        font.pixelSize: Typography.title
                        font.bold: true
                    }

                    ToolButton {
                        text: "×"
                        onClicked: detailsDrawer.close()
                        Accessible.name: qsTr("关闭会话详情")
                    }
                }

                Avatar {
                    Layout.alignment: Qt.AlignHCenter
                    name: root.controller.selectedConversationTitle
                    avatarColor: Theme.accent
                    size: Tokens.avatarProfile
                }

                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: root.controller.selectedConversationTitle
                    color: Theme.textPrimary
                    font.pixelSize: Typography.title
                    font.bold: true
                }

                RowLayout {
                    Layout.fillWidth: true

                    Button {
                        Layout.fillWidth: true
                        text: qsTr("静音")
                        onClicked: root.controller.toggleMuted(
                                       root.controller.selectedConversationIndex)
                    }

                    Button {
                        Layout.fillWidth: true
                        text: qsTr("搜索")
                        onClicked: {
                            detailsDrawer.close()
                            root.setSearchVisible(true)
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: Theme.divider
                }

                Label {
                    visible: root.controller.selectedConversationIsGroup
                    text: qsTr("群成员")
                    color: Theme.textPrimary
                    font.pixelSize: Typography.titleSmall
                    font.bold: true
                }

                Rectangle {
                    visible: root.controller.selectedConversationIsGroup
                    Layout.fillWidth: true
                    Layout.preferredHeight: 72
                    radius: Tokens.radiusMedium
                    color: Theme.surfaceMuted

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: Tokens.space3
                        spacing: Tokens.space3

                        Avatar {
                            name: qsTr("我")
                            avatarColor: Theme.accent
                            size: Tokens.avatarMedium
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Tokens.space1

                            Label {
                                text: qsTr("成员列表待同步")
                                color: Theme.textPrimary
                                font.pixelSize: Typography.body
                                font.bold: true
                            }

                            Label {
                                Layout.fillWidth: true
                                text: qsTr("群成员接口稳定后在此展示完整列表。")
                                color: Theme.textSecondary
                                font.pixelSize: Typography.bodySmall
                                wrapMode: Text.Wrap
                            }
                        }
                    }
                }

                Label {
                    text: qsTr("共享内容")
                    color: Theme.textPrimary
                    font.pixelSize: Typography.titleSmall
                    font.bold: true
                }

                EmptyState {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    iconText: "▦"
                    title: qsTr("暂无共享内容")
                    description: qsTr("文件、图片和链接将在相应能力接入后显示。")
                }
            }
        }
    }
}
