cmake_minimum_required(VERSION 3.24)

project(wimi-client VERSION 0.1.0 LANGUAGES CXX)

option(WIMI_ENABLE_LIVE_TESTS
    "Build opt-in tests that connect to a running local WIMI stack" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.10 REQUIRED COMPONENTS Network Protobuf Quick QuickControls2 Sql)

qt_standard_project_setup(REQUIRES 6.8)

add_library(wimi-client-protocol STATIC)
set_target_properties(wimi-client-protocol PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
qt_add_protobuf(wimi-client-protocol
    PROTO_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/../server/common/proto/tcp_message.proto
    OUTPUT_DIRECTORY
        ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_sources(wimi-client-protocol PRIVATE
    src/adapters/connection_gateway/ProtobufPacketCodec.cc
    src/adapters/connection_gateway/ProtobufPacketCodec.h
)
target_include_directories(wimi-client-protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(wimi-client-protocol PUBLIC Qt6::Protobuf)

set(WIMI_LINUX_DESKTOP FALSE)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
    find_package(Qt6 6.10 REQUIRED COMPONENTS DBus)
    set(WIMI_LINUX_DESKTOP TRUE)
endif()

qt_add_executable(wimi-client
    src/main.cc
)

qt_add_resources(wimi-client wimi-client-icons
    PREFIX "/icons"
    BASE resources/icons
    FILES
        resources/icons/wimi-client.svg
        resources/icons/ui/back.svg
        resources/icons/ui/chats.svg
        resources/icons/ui/compose.svg
        resources/icons/ui/contacts.svg
        resources/icons/ui/moon.svg
        resources/icons/ui/more.svg
        resources/icons/ui/muted.svg
        resources/icons/ui/pin.svg
        resources/icons/ui/requests.svg
        resources/icons/ui/search.svg
        resources/icons/ui/send.svg
        resources/icons/ui/settings.svg
)

set(WIMI_QML_SINGLETONS
    qml/Theme.qml
    qml/Tokens.qml
    qml/Typography.qml
    qml/Icons.qml
)

set_source_files_properties(${WIMI_QML_SINGLETONS}
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE
)

set(WIMI_QML_MODULE_OPTIONS)
if(NOT ANDROID AND NOT IOS)
    list(APPEND WIMI_QML_MODULE_OPTIONS NO_IMPORT_SCAN)
endif()

qt_add_qml_module(wimi-client
    URI WimiClient
    VERSION 1.0
    ${WIMI_QML_MODULE_OPTIONS}
    QML_FILES
        ${WIMI_QML_SINGLETONS}
        qml/Main.qml
        qml/AdaptiveShell.qml
        qml/AppNavigationRail.qml
        qml/AppBottomNavigation.qml
        qml/AppIcon.qml
        qml/Avatar.qml
        qml/StatusBadge.qml
        qml/ConnectionBanner.qml
        qml/EmptyState.qml
        qml/ChatListPage.qml
        qml/ConversationRow.qml
        qml/ConversationPage.qml
        qml/MessageBubble.qml
        qml/MessageComposer.qml
        qml/ContactsPage.qml
        qml/RequestCard.qml
        qml/RequestsPage.qml
        qml/SettingsPage.qml
        qml/AuthPage.qml
    SOURCES
        src/app/AgentBridge.cc
        src/app/AgentBridge.h
        src/app/AppController.cc
        src/app/AppController.h
        src/domain/ClientSnapshot.h
        src/models/ConversationListModel.cc
        src/models/ConversationListModel.h
        src/models/ContactListModel.cc
        src/models/ContactListModel.h
        src/models/MessageListModel.cc
        src/models/MessageListModel.h
        src/models/RequestListModel.cc
        src/models/RequestListModel.h
        src/ports/IClientRepository.h
        src/ports/IPlatformServices.h
        src/platform/PlatformServicesFactory.cc
        src/adapters/fake/FakeScenarioRepository.cc
        src/adapters/fake/FakeScenarioRepository.h
        src/adapters/sqlite/SqliteClientRepository.cc
        src/adapters/sqlite/SqliteClientRepository.h
        src/adapters/connection_gateway/TcpFrameCodec.cc
        src/adapters/connection_gateway/TcpFrameCodec.h
        src/adapters/connection_gateway/ClientProtocol.h
        src/adapters/connection_gateway/ConnectionGatewayClient.cc
        src/adapters/connection_gateway/ConnectionGatewayClient.h
        src/adapters/gate/GateHttpClient.cc
        src/adapters/gate/GateHttpClient.h
)

target_include_directories(wimi-client PRIVATE
    src
    src/app
)

target_link_libraries(wimi-client PRIVATE
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
    Qt6::Sql
    wimi-client-protocol
)

if(ANDROID)
    set_target_properties(wimi-client PROPERTIES
        QT_ANDROID_APP_NAME "WIMI"
        QT_ANDROID_PACKAGE_NAME "org.wimi.client"
        QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/packaging/android"
        QT_ANDROID_VERSION_CODE 1
        QT_ANDROID_VERSION_NAME "${PROJECT_VERSION}"
    )
endif()

include(GNUInstallDirs)
if(WIMI_LINUX_DESKTOP)
    target_sources(wimi-client PRIVATE
        src/platform/linux/LinuxDesktopServices.cc
        src/platform/linux/LinuxDesktopServices.h
    )
    target_link_libraries(wimi-client PRIVATE Qt6::DBus)
    install(FILES packaging/linux/wimi-client.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
    )
    install(FILES resources/icons/wimi-client.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
    )
endif()

install(TARGETS wimi-client
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.10 REQUIRED COMPONENTS Test)

    add_executable(wimi-client-model-tests
        tests/unit/ClientModelsTest.cc
        src/app/AgentBridge.cc
        src/app/AgentBridge.h
        src/app/AppController.cc
        src/app/AppController.h
        src/domain/ClientSnapshot.h
        src/models/ConversationListModel.cc
        src/models/ConversationListModel.h
        src/models/ContactListModel.cc
        src/models/ContactListModel.h
        src/models/MessageListModel.cc
        src/models/MessageListModel.h
        src/models/RequestListModel.cc
        src/models/RequestListModel.h
        src/ports/IClientRepository.h
        src/ports/IPlatformServices.h
        src/platform/PlatformServicesFactory.cc
        src/adapters/fake/FakeScenarioRepository.cc
        src/adapters/fake/FakeScenarioRepository.h
        src/adapters/sqlite/SqliteClientRepository.cc
        src/adapters/sqlite/SqliteClientRepository.h
        src/adapters/connection_gateway/TcpFrameCodec.cc
        src/adapters/connection_gateway/TcpFrameCodec.h
        src/adapters/connection_gateway/ClientProtocol.h
        src/adapters/connection_gateway/ConnectionGatewayClient.cc
        src/adapters/connection_gateway/ConnectionGatewayClient.h
        src/adapters/gate/GateHttpClient.cc
        src/adapters/gate/GateHttpClient.h
    )
    target_include_directories(wimi-client-model-tests PRIVATE
        src
    )
    target_link_libraries(wimi-client-model-tests PRIVATE
        Qt6::Qml
        Qt6::Network
        Qt6::Sql
        Qt6::Test
        wimi-client-protocol
    )
    if(WIMI_LINUX_DESKTOP)
        target_sources(wimi-client-model-tests PRIVATE
            src/platform/linux/LinuxDesktopServices.cc
            src/platform/linux/LinuxDesktopServices.h
        )
        target_link_libraries(wimi-client-model-tests PRIVATE Qt6::DBus)
    endif()
    add_test(NAME client-models COMMAND wimi-client-model-tests)

    add_executable(wimi-client-network-tests
        tests/unit/ClientNetworkTest.cc
        src/adapters/connection_gateway/ClientProtocol.h
        src/adapters/connection_gateway/ConnectionGatewayClient.cc
        src/adapters/connection_gateway/ConnectionGatewayClient.h
        src/adapters/connection_gateway/TcpFrameCodec.cc
        src/adapters/connection_gateway/TcpFrameCodec.h
        src/adapters/gate/GateHttpClient.cc
        src/adapters/gate/GateHttpClient.h
    )
    target_include_directories(wimi-client-network-tests PRIVATE src)
    target_link_libraries(wimi-client-network-tests PRIVATE
        Qt6::Network
        Qt6::Test
        wimi-client-protocol
    )
    add_test(NAME client-network COMMAND wimi-client-network-tests)

    if(WIMI_ENABLE_LIVE_TESTS)
        add_executable(wimi-client-live-network-tests
            tests/integration/ClientLiveNetworkTest.cc
            src/adapters/connection_gateway/ClientProtocol.h
            src/adapters/connection_gateway/ConnectionGatewayClient.cc
            src/adapters/connection_gateway/ConnectionGatewayClient.h
            src/adapters/connection_gateway/TcpFrameCodec.cc
            src/adapters/connection_gateway/TcpFrameCodec.h
            src/adapters/gate/GateHttpClient.cc
            src/adapters/gate/GateHttpClient.h
        )
        target_include_directories(wimi-client-live-network-tests PRIVATE src)
        target_link_libraries(wimi-client-live-network-tests PRIVATE
            Qt6::Network
            Qt6::Test
            wimi-client-protocol
        )
        add_test(NAME client-live-network
            COMMAND wimi-client-live-network-tests)
        set_tests_properties(client-live-network PROPERTIES
            LABELS integration
            TIMEOUT 30
        )
    endif()

    foreach(test_case IN ITEMS
        "compact-light;390;844;light;1"
        "medium-dark;768;1024;dark;1"
        "expanded-light;1280;800;light;1"
        "expanded-dark-scaled;1280;800;dark;2"
    )
        list(GET test_case 0 test_name)
        list(GET test_case 1 test_width)
        list(GET test_case 2 test_height)
        list(GET test_case 3 test_theme)
        list(GET test_case 4 test_scale)

        add_test(
            NAME ui-smoke-${test_name}
            COMMAND wimi-client
                --smoke-test
                --scenario=normal
                --width=${test_width}
                --height=${test_height}
                --theme=${test_theme}
                --assert-populated=conversationList
        )
        set_tests_properties(ui-smoke-${test_name} PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_SCALE_FACTOR=${test_scale}"
            TIMEOUT 10
        )
    endforeach()

    foreach(test_case IN ITEMS
        "compact-landscape;normal;chats;844;390;light"
        "compact-conversation;long-content;chats;390;844;dark"
        "contacts;friend-requests;contacts;1280;800;light"
        "requests;friend-requests;requests;1280;800;light"
        "settings;normal;settings;390;844;dark"
        "auth-expired;auth-expired;chats;390;844;dark"
        "large-history;large-history;chats;1280;800;dark"
    )
        list(GET test_case 0 test_name)
        list(GET test_case 1 test_scenario)
        list(GET test_case 2 test_section)
        list(GET test_case 3 test_width)
        list(GET test_case 4 test_height)
        list(GET test_case 5 test_theme)

        set(extra_arguments)
        if(test_name STREQUAL "compact-conversation"
           OR test_name STREQUAL "large-history")
            list(APPEND extra_arguments --open-conversation)
        endif()
        if(test_name STREQUAL "compact-landscape")
            list(APPEND extra_arguments --assert-populated=conversationList)
        elseif(test_name STREQUAL "compact-conversation"
               OR test_name STREQUAL "large-history")
            list(APPEND extra_arguments --assert-populated=timeline)
        elseif(test_name STREQUAL "contacts")
            list(APPEND extra_arguments --assert-populated=contactList)
        elseif(test_name STREQUAL "requests")
            list(APPEND extra_arguments --assert-populated=requestList)
        endif()

        add_test(
            NAME ui-smoke-${test_name}
            COMMAND wimi-client
                --smoke-test
                --scenario=${test_scenario}
                --section=${test_section}
                --width=${test_width}
                --height=${test_height}
                --theme=${test_theme}
                ${extra_arguments}
        )
        set_tests_properties(ui-smoke-${test_name} PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
            TIMEOUT 10
        )
    endforeach()

    add_test(
        NAME ui-smoke-sqlite-restore
        COMMAND wimi-client
            --smoke-test
            --repository=sqlite
            --database=:memory:
            --width=1280
            --height=800
            --theme=light
            --assert-populated=conversationList
    )
    set_tests_properties(ui-smoke-sqlite-restore PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
        TIMEOUT 10
    )

    add_test(
        NAME ui-smoke-network-auth
        COMMAND wimi-client
            --smoke-test
            --gate-url=http://127.0.0.1:1
            --database=:memory:
            --width=390
            --height=844
            --theme=dark
    )
    set_tests_properties(ui-smoke-network-auth PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
        TIMEOUT 10
    )
endif()
