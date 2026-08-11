#include <QGuiApplication>
#include <QDebug>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QQuickStyle>
#include <QSysInfo>
#include <QTimer>

namespace
{

bool HasArgument(const QStringList& arguments, const QString& expected) { return arguments.contains(expected); }

QString ArgumentValue(const QStringList& arguments, const QString& name)
{
    const QString prefix = QStringLiteral("--") + name + QStringLiteral("=");
    for (const auto& argument : arguments) {
        if (argument.startsWith(prefix)) {
            return argument.mid(prefix.size());
        }
    }
    return {};
}

void ConfigureWslRendering()
{
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    const bool rendererOverridden =
        qEnvironmentVariableIsSet("QSG_RHI_BACKEND") || qEnvironmentVariableIsSet("QT_QUICK_BACKEND");
    if (!rendererOverridden && QSysInfo::kernelVersion().contains(QStringLiteral("microsoft"), Qt::CaseInsensitive)) {
        qputenv("QSG_RHI_BACKEND", QByteArrayLiteral("opengl"));
        if (!qEnvironmentVariableIsSet("LIBGL_ALWAYS_SOFTWARE")) {
            qputenv("LIBGL_ALWAYS_SOFTWARE", QByteArrayLiteral("1"));
        }
    }
#endif
}

} // namespace

int main(int argc, char* argv[])
{
    ConfigureWslRendering();
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QGuiApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("WIMI"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QCoreApplication::setOrganizationName(QStringLiteral("WIMI"));
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    QGuiApplication::setDesktopFileName(QStringLiteral("wimi-client"));
#endif
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/wimi-client.svg")));

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &application,
        [] { QCoreApplication::exit(EXIT_FAILURE); }, Qt::QueuedConnection);
    engine.loadFromModule(QStringLiteral("WimiClient"), QStringLiteral("Main"));

    const QString screenshotPath = ArgumentValue(application.arguments(), QStringLiteral("screenshot"));
    if (!screenshotPath.isEmpty()) {
        QTimer::singleShot(500, &application, [&application, &engine, screenshotPath] {
            auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().value(0));
            const bool saved = window != nullptr && window->grabWindow().save(screenshotPath);
            application.exit(saved ? EXIT_SUCCESS : EXIT_FAILURE);
        });
    } else if (HasArgument(application.arguments(), QStringLiteral("--smoke-test"))) {
        const QString populatedObjectName = ArgumentValue(application.arguments(), QStringLiteral("assert-populated"));
        QTimer::singleShot(
            populatedObjectName.isEmpty() ? 300 : 500, &application, [&application, &engine, populatedObjectName] {
                if (!populatedObjectName.isEmpty()) {
                    QObject* root = engine.rootObjects().value(0);
                    QObject* object = root == nullptr ? nullptr : root->findChild<QObject*>(populatedObjectName);
                    const int count = object == nullptr ? 0 : object->property("count").toInt();
                    if (count <= 0) {
                        qCritical() << "Smoke assertion failed: expected populated" << populatedObjectName;
                        application.exit(EXIT_FAILURE);
                        return;
                    }
                }
                application.quit();
            });
    }

    return application.exec();
}
