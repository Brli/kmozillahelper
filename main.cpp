/*****************************************************************

Copyright (C) 2009 Lubos Lunak <l.lunak@suse.cz>
Copyright (C) 2017 Fabian Vogt <fabian@ritter-vogt.de>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

******************************************************************/

#include "main.h"

#include <cassert>
#include <sys/types.h>
#include <unistd.h>

#include <iostream>

#include <QApplication>
#include <QCommandLineParser>
#include <QFileDialog>
#include <QHash>
#include <QIcon>
#include <QMimeDatabase>
#include <QWindow>

#include <KAboutData>
#include <KApplicationTrader>
#include <KConfigGroup>
#include <KIO/ApplicationLauncherJob>
#include <KIO/CommandLauncherJob>
#include <KIO/JobUiDelegate>
#include <KIO/OpenUrlJob>
#include <KLocalizedString>
#include <KNotification>
#include <KNotificationJobUiDelegate>
#include <KOpenWithDialog>
#include <KProcess>
#include <KProtocolInfo>
#include <KRecentDocument>
#include <KSharedConfig>
#include <KShell>
#include <KWindowSystem>

// #define DEBUG_KDE

#define HELPER_VERSION 6
#define APP_HELPER_VERSION "5.99.0"

using namespace Qt::Literals::StringLiterals;

int main(int argc, char *argv[])
{
    // Avoid getting started by the session manager
    qunsetenv("SESSION_MANAGER");

    QApplication app(argc, argv);

    // Check whether we're called from Firefox or Thunderbird
    QString appname = i18n("Mozilla Firefox");
    QString parent = QFile::symLinkTarget(u"/proc/%1/exe"_s);
    if (parent.contains("thunderbird"_L1))
        appname = i18n("Mozilla Thunderbird");

    // This shows on file dialogs
    KAboutData about(u"kmozillahelper"_s), appname_s, APP_HELPER_VERSION_s;
    about.setBugAddress("https://bugzilla.opensuse.org/enter_bug.cgi");
    KAboutData::setApplicationData(about);
    QApplication::setQuitOnLastWindowClosed(false);

    QCommandLineParser parser;
    about.setupCommandLine(&parser);

    app.setQuitOnLastWindowClosed(false);

    Helper helper;

    app.installEventFilter(&helper);

    return app.exec();
}

Helper::Helper() : notifier(STDIN_FILENO, QSocketNotifier::Read), arguments_read(false)
{
    connect(&notifier, &QSocketNotifier::activated, this, &Helper::readCommand);
}

static bool runApplication(const KService::Ptr &service, const QList<QUrl> &urls)
{
    auto *job = new KIO::ApplicationLauncherJob(service);
    job->setUrls(urls);
    job->setUiDelegate(new KNotificationJobUiDelegate(KJobUiDelegate::AutoErrorHandlingEnabled));
    job->start();
    return true;
}

void Helper::readCommand()
{
    QString command = readLine();
    if (!std::cin.good())
    {
#ifdef DEBUG_KDE
        std::cerr << "EOF, exiting." << std::endl;
#endif
        QCoreApplication::exit();
        return;
    }

    /* Allow multiple commands at once.
       Firefox nests the event loop in the same way we do,
       so if a file dialog is open, another command may arrive which we handle
       in our nested event loop...
    // For now we only allow one command at once.
    // We need to do this as dialogs spawn their own eventloop and thus they get nested...
    notifier.setEnabled(false); */

#ifdef DEBUG_KDE
    std::cerr << "COMMAND: " << command.toStdString() << std::endl;
#endif
    bool status;
    if (command == u"CHECK"_s)
        status = handleCheck();
    else if (command == u"GETPROXY"_s)
        status = handleGetProxy();
    else if (command == u"HANDLEREXISTS"_s)
        status = handleHandlerExists();
    else if (command == u"GETFROMEXTENSION"_s)
        status = handleGetFromExtension();
    else if (command == u"GETFROMTYPE"_s)
        status = handleGetFromType();
    else if (command == u"GETAPPDESCFORSCHEME"_s)
        status = handleGetAppDescForScheme();
    else if (command == u"APPSDIALOG"_s)
        status = handleAppsDialog();
    else if (command == u"GETOPENFILENAME"_s)
        status = handleGetOpenOrSaveX(false, false);
    else if (command == u"GETOPENURL"_s)
        status = handleGetOpenOrSaveX(true, false);
    else if (command == u"GETSAVEFILENAME"_s)
        status = handleGetOpenOrSaveX(false, true);
    else if (command == u"GETSAVEURL"_s)
        status = handleGetOpenOrSaveX(true, true);
    else if (command == u"GETDIRECTORYFILENAME"_s)
        status = handleGetDirectoryX(false);
    else if (command == u"GETDIRECTORYURL"_s)
        status = handleGetDirectoryX(true);
    else if (command == u"OPEN"_s)
        status = handleOpen();
    else if (command == u"REVEAL"_s)
        status = handleReveal();
    else if (command == u"RUN"_s)
        status = handleRun();
    else if (command == u"GETDEFAULTFEEDREADER"_s)
        status = handleGetDefaultFeedReader();
    else if (command == u"OPENMAIL"_s)
        status = handleOpenMail();
    else if (command == u"OPENNEWS"_s)
        status = handleOpenNews();
    else if (command == u"ISDEFAULTBROWSER"_s)
        status = handleIsDefaultBrowser();
    else if (command == u"SETDEFAULTBROWSER"_s)
        status = handleSetDefaultBrowser();
    else if (command == u"DOWNLOADFINISHED"_s)
        status = handleDownloadFinished();
    else
    {
        std::cerr << "Unknown command for KDE helper: " << command.toStdString() << std::endl;
        status = false;
    }
    // status done as \1 (==ok) and \0 (==not ok), because otherwise this cannot happen
    // in normal data (\ is escaped otherwise)
    outputLine(QVariant(status).toInt() ? u"1"_s: u"0"_s, false); // do not escape

    /* See comment on setEnabled above
    notifier.setEnabled(true); */
}

bool Helper::handleCheck()
{
    if (!readArguments(1))
        return false;
    int version = getArgument().toInt(); // requested version
    if (!allArgumentsUsed())
        return false;
    if (version <= HELPER_VERSION) // we must have the exact requested version
        return true;
    std::cerr << "KDE helper version too old." << std::endl;
    return false;
}

bool Helper::handleGetProxy()
{
    if (!readArguments(1))
        return false;
    QUrl url = QUrl::fromUserInput(getArgument());
    if (!allArgumentsUsed())
        return false;
    QString proxy;
    if (proxy.isEmpty() || proxy == u"DIRECT"_s) // TODO return DIRECT if empty?
    {
        outputLine(u"DIRECT"_s);
        return true;
    }
    QUrl proxyurl = QUrl::fromUserInput(proxy);
    if (proxyurl.isValid())
    { // firefox wants this format
        outputLine("PROXY"_L1
                   " "_L1 +
                   proxyurl.host() + ":"_L1 + QString::number(proxyurl.port()));
        // TODO there is also "SOCKS " type
        return true;
    }
    return false;
}

bool Helper::handleHandlerExists()
{
    // Cache protocols types to avoid causing Thunderbird to hang (https://bugzilla.suse.com/show_bug.cgi?id=1037806).
    static QHash<QString, bool> known_protocols;

    if (!readArguments(1))
        return false;
    QString protocol = getArgument();
    if (!allArgumentsUsed())
        return false;

    auto it(known_protocols.find(protocol));
    if (it == known_protocols.end())
        it = known_protocols.insert(protocol, KProtocolInfo::isHelperProtocol(protocol));

    if (*it)
        return true;

    return KApplicationTrader::preferredService("x-scheme-handler/"_L1 + protocol) != nullptr;
}

bool Helper::handleGetFromExtension()
{
    if (!readArguments(1))
        return false;
    QString ext = getArgument();
    if (!allArgumentsUsed())
        return false;
    if (!ext.isEmpty())
    {
        QList<QMimeType> mimeList = QMimeDatabase().mimeTypesForFileName("foo."_L1 + ext);
        for (const QMimeType &mime : mimeList)
            if (mime.isValid())
                return writeMimeInfo(mime);
    }
    return false;
}

bool Helper::handleGetFromType()
{
    if (!readArguments(1))
        return false;
    QString type = getArgument();
    if (!allArgumentsUsed())
        return false;
    QMimeType mime = QMimeDatabase().mimeTypeForName(type);
    if (mime.isValid())
        return writeMimeInfo(mime);
    // firefox also asks for protocol handlers using getfromtype
    QString app = getAppForProtocol(type);
    if (!app.isEmpty())
    {
        outputLine(type);
        outputLine(type); // TODO probably no way to find a good description
        outputLine(app);
        return true;
    }
    return false;
}

bool Helper::writeMimeInfo(QMimeType mime)
{
    KService::Ptr service = KApplicationTrader::preferredService(mime.name());
    if (service)
    {
        outputLine(mime.name());
        outputLine(mime.comment());
        outputLine(service->name());
        return true;
    }
    return false;
}

bool Helper::handleGetAppDescForScheme()
{
    if (!readArguments(1))
        return false;
    QString scheme = getArgument();
    if (!allArgumentsUsed())
        return false;
    QString app = getAppForProtocol(scheme);
    if (!app.isEmpty())
    {
        outputLine(app);
        return true;
    }
    return false;
}

bool Helper::handleAppsDialog()
{
    if (!readArguments(1))
        return false;
    QString title = getArgument();
    long wid = getArgumentParent();
    if (!allArgumentsUsed())
        return false;
    KOpenWithDialog dialog(nullptr);
    if (!title.isEmpty())
        dialog.setWindowTitle(title);
    dialog.hideNoCloseOnExit();
    dialog.hideRunInTerminal(); // TODO
    if (wid != 0)
    {
        dialog.setAttribute(Qt::WA_NativeWindow, true);
        KWindowSystem::setMainWindow(dialog.windowHandle(), wid);
    }
    if (dialog.exec())
    {
        KService::Ptr service = dialog.service();
        QString command;
        if (service)
            command = service->exec();
        else if (!dialog.text().isEmpty())
            command = dialog.text();
        else
            return false;
        command = command.split(u" "_s).first(); // only the actual command
        command = QStandardPaths::findExecutable(command);
        if (command.isEmpty())
            return false;
        outputLine(QUrl::fromUserInput(command).url());
        return true;
    }
    return false;
}

QStringList Helper::convertToNameFilters(const QString &input)
{
    QStringList ret;

    // Filters separated by newline
    for (auto &filter : input.split('\n'_L1))
    {
        // Filer exp and name separated by '|'.
        // TODO: Is it possible that | appears in either of those?
        auto data = filter.split('|'_L1);

        if (data.length() == 1)
            ret.append(QStringLiteral("%0 Files(%0)").arg(data[0]));
        else if (data.length() >= 2)
            ret.append(QStringLiteral("%0 (%1)(%1)").arg(data[1]).arg(data[0]));
    }

    return ret;
}

bool Helper::handleGetOpenOrSaveX(bool url, bool save)
{
    if (!readArguments(4))
        return false;
    QUrl defaultPath = QUrl::fromLocalFile(getArgument());
    // Use dialog.nameFilters() instead of filtersParsed as setNameFilters does some syntax changes
    QStringList filtersParsed = convertToNameFilters(getArgument());
    int selectFilter = getArgument().toInt();
    QString title = getArgument();
    bool multiple = save ? false : isArgument(u"MULTIPLE"_s);
    this->wid = getArgumentParent();
    if (!allArgumentsUsed())
        return false;

    if (title.isEmpty())
        title = save ? i18n("Save") : i18n("Open");

    QFileDialog dialog(nullptr, title, defaultPath.path());

    dialog.selectFile(defaultPath.fileName());
    dialog.setNameFilters(filtersParsed);
    dialog.setOption(QFileDialog::DontConfirmOverwrite, false);
    dialog.setAcceptMode(save ? QFileDialog::AcceptSave : QFileDialog::AcceptOpen);

    if (save)
        dialog.setFileMode((QFileDialog::AnyFile));
    else
        dialog.setFileMode(multiple ? QFileDialog::ExistingFiles : QFileDialog::ExistingFile);

    if (selectFilter >= 0 && selectFilter >= dialog.nameFilters().size())
        dialog.selectNameFilter(dialog.nameFilters().at(selectFilter));

        // If url == false only allow local files. Impossible to do with Qt < 5.6...
#if (QT_VERSION >= QT_VERSION_CHECK(5, 6, 0))
    if (url == false)
        dialog.setSupportedSchemes(QStringList(u"file"_s));
#endif

    // Run dialog
    if (dialog.exec() != QDialog::Accepted)
        return false;

    int usedFilter = dialog.nameFilters().indexOf(dialog.selectedNameFilter());

    if (url)
    {
        QList<QUrl> result = dialog.selectedUrls();
        result.removeAll(QUrl());
        if (!result.isEmpty())
        {
            outputLine(QStringLiteral("%0").arg(usedFilter));
            for (const QUrl &url : result)
                outputLine(url.url());
            return true;
        }
    }
    else
    {
        QStringList result = dialog.selectedFiles();
        result.removeAll(QString());
        if (!result.isEmpty())
        {
            outputLine(QStringLiteral("%0").arg(usedFilter));
            for (const QString &str : result)
                outputLine(str);
            return true;
        }
    }
    return false;
}

bool Helper::handleGetDirectoryX(bool url)
{
    if (!readArguments(2))
        return false;
    QString startDir = getArgument();
    QString title = getArgument();
    this->wid = getArgumentParent();
    if (!allArgumentsUsed())
        return false;

    if (url)
    {
        QUrl result = QFileDialog::getExistingDirectoryUrl(nullptr, title, QUrl::fromLocalFile(startDir));
        if (result.isValid())
        {
            outputLine(result.url());
            return true;
        }
    }
    else
    {
        QString result = QFileDialog::getExistingDirectory(nullptr, title, startDir);
        if (!result.isEmpty())
        {
            outputLine(result);
            return true;
        }
    }
    return false;
}

bool Helper::handleOpen()
{
    if (!readArguments(1))
        return false;
    QUrl url = QUrl::fromUserInput(getArgument());
    QString mime;
    if (isArgument(u"MIMETYPE"_s))
        mime = getArgument();
    if (!allArgumentsUsed())
        return false;
    // try to handle the case when the server has broken mimetypes and e.g. claims something is application/octet-stream
    QMimeType mimeType = QMimeDatabase().mimeTypeForName(mime);
    if (!mime.isEmpty() && mimeType.isValid() && KApplicationTrader::preferredService(mimeType.name()))
    {
        KIO::OpenUrlJob *job = new KIO::OpenUrlJob(url, mime);
        job->setUiDelegate(new KNotificationJobUiDelegate(KJobUiDelegate::AutoErrorHandlingEnabled));
        job->setDeleteTemporaryFile(true); // delete the file, once the client exits
        job->start();
        return true;
    }
    else
    {
        (void)new KIO::OpenUrlJob(url, nullptr);
        //    QObject::connect(run, SIGNAL(finished()), &app, SLOT(openDone()));
        //    QObject::connect(run, SIGNAL(error()), &app, SLOT(openDone()));
        return true; // TODO check for errors?
    }
}

bool Helper::handleReveal()
{
    if (!readArguments(1))
        return false;
    QString path = getArgument();
    if (!allArgumentsUsed())
        return false;
    const KService::List apps = KApplicationTrader::queryByMimeType(u"inode/directory"_s);
    if (apps.size() != 0)
    {
        QString command = apps.at(0)->exec().split(u" "_s).first(); // only the actual command
        if (command == u"dolphin"_s || command == u"konqueror"_s)
        {
            command = QStandardPaths::findExecutable(command);
            if (command.isEmpty())
                return false;
            return KProcess::startDetached(command, QStringList() << u"--select"_s << path);
        }
    }
    QFileInfo info(path);
    QString dir = info.dir().path();
    (void)new KIO::OpenUrlJob(QUrl::fromLocalFile(dir), nullptr); // TODO parent
    return true;                                               // TODO check for errors?
}

bool Helper::handleRun()
{
    if (!readArguments(2))
        return false;
    QString app = getArgument();
    QString arg = getArgument();
    if (!allArgumentsUsed())
        return false;
    auto job = new KIO::CommandLauncherJob(KShell::quoteArg(app), {KShell::quoteArg(arg)});
    job->setUiDelegate(new KNotificationJobUiDelegate(KJobUiDelegate::AutoErrorHandlingEnabled));
    job->start();
    return true;
}

bool Helper::handleGetDefaultFeedReader()
{
    if (!readArguments(0))
        return false;
    // firefox wants the full path
    QString reader = QStandardPaths::findExecutable(u"akregator"_s); // TODO there is no KDE setting for this
    if (!reader.isEmpty())
    {
        outputLine(reader);
        return true;
    }
    return false;
}

bool Helper::handleOpenMail()
{
    if (!readArguments(0))
        return false;
    // this is based on ktoolinvocation_x11.cpp, there is no API for this
    KConfig config(u"emaildefaults"_s);
    QString groupname = KConfigGroup(&config, u"Defaults"_s).readEntry("Profile", "Default");
    KConfigGroup group(&config, QStringLiteral("PROFILE_%1").arg(groupname));
    QString command = group.readPathEntry("EmailClient", QString());
    if (command.isEmpty())
        command = u"kmail"_s;
    if (group.readEntry("TerminalClient", false))
    {
        QString terminal =
            KConfigGroup(KSharedConfig::openConfig(), u"General"_s).readPathEntry("TerminalApplication", u"konsole"_s);
        command = terminal + " -e "_L1 + command;
    }
    KService::Ptr mail = KService::serviceByDesktopName(command.split(u" "_s).first());
    if (mail)
    {
        return runApplication(mail, QList<QUrl>()); // TODO parent
    }
    return false;
}

bool Helper::handleOpenNews()
{
    if (!readArguments(0))
        return false;
    KService::Ptr news = KService::serviceByDesktopName(u"knode"_s); // TODO there is no KDE setting for this
    if (news)
    {
        return runApplication(news, QList<QUrl>()); // TODO parent
    }
    return false;
}

bool Helper::handleIsDefaultBrowser()
{
    if (!readArguments(0))
        return false;
    QString browser = KConfigGroup(KSharedConfig::openConfig(u"kdeglobals"_s), u"General"_s).readEntry("BrowserApplication");
    return browser == u"MozillaFirefox"_s || browser == u"MozillaFirefox.desktop"_s || browser == u"!firefox"_s ||
           browser == u"!/usr/bin/firefox"_s || browser == u"firefox"_s || browser == u"firefox.desktop"_s;
}

bool Helper::handleSetDefaultBrowser()
{
    if (!readArguments(1))
        return false;
    bool alltypes = (getArgument() == u"ALLTYPES"_s);
    if (!allArgumentsUsed())
        return false;
    KConfigGroup(KSharedConfig::openConfig(u"kdeglobals"_s), u"General"_s).writeEntry(u"BrowserApplication"_s, u"firefox"_s);
    if (alltypes)
    {
        // TODO there is no API for this and it is a bit complex
    }
    return true;
}

bool Helper::handleDownloadFinished()
{
    if (!readArguments(1))
        return false;
    QString download = getArgument();
    if (!allArgumentsUsed())
        return false;
    // TODO cheat a bit due to i18n freeze - the strings are in the .notifyrc file,
    // taken from KGet, but the notification itself needs the text too.
    // So create it from there.
    KConfig cfg(u"kmozillahelper.notifyrc"_s, KConfig::FullConfig, QStandardPaths::AppDataLocation);
    QString message = KConfigGroup(&cfg, u"Event/downloadfinished"_s).readEntry("Comment");
    KNotification::event("downloadfinished"_L1, download + " : "_L1 + message);
    return true;
}

QString Helper::getAppForProtocol(const QString &protocol)
{
    /* Inspired by kio's krun.cpp */
    const KService::Ptr service = KApplicationTrader::preferredService("x-scheme-handler/"_L1 + protocol);
    if (service)
        return service->name();

    /* Some KDE services (e.g. vnc) also support application associations.
     * Those are known as "Helper Protocols".
     * However, those aren't also registered using fake mime types and there
     * is no link to a .desktop file...
     * So we need to query for the service to use and then find the .desktop
     * file for that application by comparing the Exec values. */

    if (!KProtocolInfo::isHelperProtocol(protocol))
        return {};

    QString exec = KProtocolInfo::exec(protocol);

    if (exec.isEmpty())
        return {};

    if (exec.contains(' '_L1))
        exec = exec.split(' '_L1).first(); // first part of command

    if (KService::Ptr service = KService::serviceByDesktopName(exec))
        return service->name();

    QString servicename;
    for (KService::Ptr service : KService::allServices())
    {
        QString exec2 = service->exec();
        if (exec2.contains(' '_L1))
            exec2 = exec2.split(' '_L1).first(); // first part of command
        if (exec == exec2)
        {
            servicename = service->name();
            break;
        }
    }

    if (servicename.isEmpty() && exec == u"kmailservice"_s) // kmailto is handled internally by kmailservice
        servicename = i18n("KDE");

    return servicename;
}

QString Helper::readLine()
{
    std::string line;
    if (!std::getline(std::cin, line))
        return {};

    QString qline = QString::fromStdString(line);
    qline.replace(u"\\n"_s, u"\n"_s);
    qline.replace(u"\\"_s
                  u"\\"_s,
                  u"\\"_s);
    return qline;
}

/* Qt just uses the QWidget* parent as transient parent for native
 * platform dialogs. This makes it impossible to make them transient
 * to a bare QWindow*. So we catch the show event for the QDialog
 * and setTransientParent here instead. */
bool Helper::eventFilter(QObject *obj, QEvent *ev)
{
    if (ev->type() == QEvent::Show && obj->inherits("QDialog"))
    {
        QWidget *widget = static_cast<QWidget *>(obj);
        if (wid != 0)
        {
            widget->setAttribute(Qt::WA_NativeWindow, true);
            KWindowSystem::setMainWindow(widget->windowHandle(), wid);
        }
    }

    return false;
}

void Helper::outputLine(QString line, bool escape)
{
    if (escape)
    {
        line.replace(u"\\"_s, u"\\_s"
                           u"\\"_s);
        line.replace(u"\n"_s, u"\\n"_s);
    }
    std::cout << line.toStdString() << std::endl;
#ifdef DEBUG_KDE
    std::cerr << "OUTPUT: " << line.toStdString() << std::endl;
#endif
}

bool Helper::readArguments(int mincount)
{
    assert(arguments.isEmpty());
    for (;;)
    {
        QString line = readLine();
        if (!std::cin.good())
        {
            arguments.clear();
            return false;
        }
        if (line == u"\\E"_s)
        {
            arguments_read = true;
            if (arguments.count() >= mincount)
                return true;
            std::cerr << "Not enough arguments for KDE helper." << std::endl;
            return false;
        }
        arguments.append(line);
    }
}

QString Helper::getArgument()
{
    assert(!arguments.isEmpty());
    return arguments.takeFirst();
}

bool Helper::isArgument(const QString &argument)
{
    if (!arguments.isEmpty() && arguments.first() == argument)
    {
        arguments.removeFirst();
        return true;
    }
    return false;
}

bool Helper::allArgumentsUsed()
{
    assert(arguments_read);
    arguments_read = false;
    if (arguments.isEmpty())
        return true;
    std::cerr << "Unused arguments for KDE helper:" << arguments.join(u" "_s).toStdString() << std::endl;
    arguments.clear();
    return false;
}

long Helper::getArgumentParent()
{
    if (isArgument(u"PARENT"_s))
        return getArgument().toLong();
    return 0;
}
