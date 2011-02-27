#include <qabstractitemview.h>
#include <qmessagebox.h>
#include <qvariant.h>
#include <qprogressdialog.h>
#include <qwaitcondition.h>
#include <qthread.h>
#include <windows.h>
#include <qtimer.h>
#include <qdebug.h>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QRegExp>
#include <qdatetime.h>
#include "qdesktopservices.h"
#include <qinputdialog.h>
#include <qfiledialog.h>
#include <qtextstream.h>
#include <qiodevice.h>
#include <qmenu.h>
#include <qtextedit.h>

#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "repository.h"
#include "job.h"
#include "progressdialog.h"
#include "settingsdialog.h"
#include "wpmutils.h"
#include "installoperation.h"
#include "downloader.h"
#include "packageversionform.h"
#include "uiutils.h"

extern HWND defaultPasswordWindow;

QMap<QString, QIcon> MainWindow::icons;
QIcon MainWindow::genericAppIcon;

class InstallThread: public QThread
{
    PackageVersion* pv;

    // 0, 1, 2 = install/uninstall
    // 3, 4 = recognize installed applications + load repositories
    // 5 = download the package and compute it's SHA1
    // 6 = test repositories
    // 7 = download all files
    // 8 = scan hard drives
    int type;

    Job* job;

    void testRepositories();
    void downloadAllFiles();
    void testOnePackage(Job* job, PackageVersion* pv);
    void scanHardDrives();
public:
    // name of the log file for type=6
    // directory for type=7
    QString logFile;

    /** computed SHA1 will be stored here (type == 5) */
    QString sha1;

    QList<InstallOperation*> install;

    InstallThread(PackageVersion* pv, int type, Job* job);

    void run();
};

void InstallThread::testOnePackage(Job *job, PackageVersion *pv)
{
    if (pv->isExternal()) {
        job->setProgress(1);
        job->complete();
        return;
    }

    Repository* r = Repository::getDefault();

    QList<InstallOperation*> ops;

    if (!job->isCancelled()) {
        job->setHint("Planning the installation");
        QList<PackageVersion*> installed = r->getInstalled();
        qDeleteAll(ops);
        ops.clear();
        QString e = pv->planInstallation(installed, ops);
        if (!e.isEmpty()) {
            job->setErrorMessage(QString(
                    "Installation planning failed: %1").
                    arg(e));
        }
        job->setProgress(0.1);
    }

    if (job->getErrorMessage().isEmpty() && !job->isCancelled()) {
        job->setHint("Installing");
        Job* instJob = job->newSubJob(0.4);
        r->process(instJob, ops);
        if (!instJob->getErrorMessage().isEmpty())
            job->setErrorMessage(instJob->getErrorMessage());
        delete instJob;

        if (!pv->installed() && job->getErrorMessage().isEmpty())
            job->setErrorMessage("Package is not installed after the installation");
    }

    if (job->getErrorMessage().isEmpty() && !job->isCancelled()) {
        job->setHint("Planning the un-installation");
        QList<PackageVersion*> installed = r->getInstalled();
        qDeleteAll(ops);
        ops.clear();
        QString e = pv->planUninstallation(installed, ops);
        if (!e.isEmpty()) {
            job->setErrorMessage(QString(
                    "Un-installation planning failed: %1").
                    arg(e));
        }
        job->setProgress(0.6);
    }

    if (job->getErrorMessage().isEmpty() && !job->isCancelled()) {
        job->setHint("Removing");
        Job* instJob = job->newSubJob(0.4);
        r->process(instJob, ops);
        if (!instJob->getErrorMessage().isEmpty())
            job->setErrorMessage(instJob->getErrorMessage());
        delete instJob;

        if (pv->installed() && job->getErrorMessage().isEmpty())
            job->setErrorMessage("Package is installed after the un-installation");
    }

    qDeleteAll(ops);
    ops.clear();

    job->complete();
}

InstallThread::InstallThread(PackageVersion *pv, int type, Job* job)
{
    this->pv = pv;
    this->type = type;
    this->job = job;
}

void InstallThread::downloadAllFiles()
{
    QFile f(this->logFile + "\\Download.log");
    QTextStream ts(&f);
    Repository* r = Repository::getDefault();
    double step = 1.0 / r->packageVersions.count();
    for (int i = 0; i < r->packageVersions.count(); i++) {
        PackageVersion* pv = r->packageVersions.at(i);

        if (job->isCancelled())
            break;

        job->setHint(QString("Downloading %1").arg(pv->toString()));
        Job* stepJob = job->newSubJob(step);
        QString to = this->logFile + "\\" + pv->package + "-" +
                     pv->version.getVersionString() + pv->getFileExtension();
        pv->downloadTo(stepJob, to);
        if (!stepJob->getErrorMessage().isEmpty())
            ts << QString("Error downloading %1: %2").
                    arg(pv->toString()).
                    arg(stepJob->getErrorMessage()) << endl;
        delete stepJob;

        if (job->isCancelled() || !job->getErrorMessage().isEmpty())
            break;
    }
    f.close();
    job->complete();
}

void InstallThread::scanHardDrives()
{
    Repository* r = Repository::getDefault();
    r->scanHardDrive(job);
}

void InstallThread::testRepositories()
{
    QFile f(this->logFile);

    if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) {
        job->setErrorMessage(QString("Failed to open the log file %1").
                arg(this->logFile));
    } else {
        QTextStream ts(&f);
        Repository* r = Repository::getDefault();
        double step = 1.0 / r->packageVersions.count();
        for (int i = 0; i < r->packageVersions.count(); i++) {
            PackageVersion* pv = r->packageVersions.at(i);

            if (job->isCancelled())
                break;

            job->setHint(QString("Testing %1").arg(pv->toString()));
            Job* stepJob = job->newSubJob(step);
            testOnePackage(stepJob, pv);
            if (!stepJob->getErrorMessage().isEmpty())
                ts << QString("Error testing %1: %2").
                        arg(pv->toString()).
                        arg(stepJob->getErrorMessage()) << endl;
            delete stepJob;

            if (job->isCancelled() || !job->getErrorMessage().isEmpty())
                break;

            job->setProgress(i * step);
        }
        f.close();
    }

    job->complete();
}

void InstallThread::run()
{
    CoInitialize(NULL);

    // qDebug() << "InstallThread::run.1";
    switch (this->type) {
    case 0:
    case 1:
    case 2:
        Repository::getDefault()->process(job, install);
        break;
    case 3:
    case 4: {
        Repository* r = Repository::getDefault();
        r->reload(job);
        break;
    }
    case 5:
        this->sha1 = pv->downloadAndComputeSHA1(job);
        break;
    case 6:
        testRepositories();
        break;
    case 7:
        downloadAllFiles();
        break;
    case 8:
        scanHardDrives();
        break;
    }

    CoUninitialize();

    // qDebug() << "InstallThread::run.2";
}

bool MainWindow::winEvent(MSG* message, long* result)
{    
    if (message->message == WM_ICONTRAY) {
        // qDebug() << "MainWindow::winEvent " << message->lParam;
        switch (message->lParam) {
            case (LPARAM) NIN_BALLOONUSERCLICK:
                this->ui->comboBoxStatus->setCurrentIndex(3);
                this->prepare();
                ((QApplication*) QApplication::instance())->
                        setQuitOnLastWindowClosed(true);
                this->showMaximized();
                this->activateWindow();
                break;
            case (LPARAM) NIN_BALLOONTIMEOUT:
                ((QApplication*) QApplication::instance())->quit();
                break;
        }
        return true;
    }
    return false;
}

void MainWindow::showDetails()
{
    PackageVersion* pv = getSelectedPackageVersion();
    if (pv) {
        PackageVersionForm* pvf = new PackageVersionForm(this->ui->tabWidget);
        pvf->fillForm(pv);
        QIcon icon = getPackageVersionIcon(pv);
        this->ui->tabWidget->addTab(pvf, icon, pv->toString());
        this->ui->tabWidget->setCurrentIndex(this->ui->tabWidget->count() - 1);
    }
}

void MainWindow::updateIcons()
{
    Repository* r = Repository::getDefault();
    for (int i = 0; i < this->ui->tableWidget->rowCount(); i++) {
        QTableWidgetItem *item = ui->tableWidget->item(i, 0);
        const QVariant v = item->data(Qt::UserRole);
        PackageVersion* pv = (PackageVersion *) v.value<void*>();
        Package* p = r->findPackage(pv->package);

        if (p) {
            if (!p->icon.isEmpty() && this->icons.contains(p->icon)) {
                QIcon icon = this->icons[p->icon];
                item->setIcon(icon);
            }
        }
    }

    for (int i = 0; i < this->ui->tabWidget->count(); i++) {
        QWidget* w = this->ui->tabWidget->widget(i);
        PackageVersionForm* pvf = dynamic_cast<PackageVersionForm*>(w);
        if (pvf) {
            pvf->updateIcons();
            QIcon icon = getPackageVersionIcon(pvf->pv);
            this->ui->tabWidget->setTabIcon(i, icon);
        }
    }
}

void MainWindow::updateStatusInDetailTabs()
{
    for (int i = 0; i < this->ui->tabWidget->count(); i++) {
        QWidget* w = this->ui->tabWidget->widget(i);
        PackageVersionForm* pvf = dynamic_cast<PackageVersionForm*>(w);
        if (pvf) {
            pvf->updateStatus();
        }
    }
}

QIcon MainWindow::getPackageVersionIcon(PackageVersion *pv)
{
    Repository* r = Repository::getDefault();
    Package* p = r->findPackage(pv->package);

    QIcon icon = MainWindow::genericAppIcon;
    if (p) {
        if (!p->icon.isEmpty() && MainWindow::icons.contains(p->icon)) {
            icon = MainWindow::icons[p->icon];
        }
    }
    return icon;
}

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setWindowTitle("Npackd");

    this->genericAppIcon = QIcon(":/images/app.png");

    this->ui->tableWidget->setEditTriggers(QTableWidget::NoEditTriggers);

    QList<QUrl*> urls = Repository::getRepositoryURLs();
    if (urls.count() == 0) {
        urls.append(new QUrl(
                "https://windows-package-manager.googlecode.com/hg/repository/Rep.xml"));
        if (WPMUtils::is64BitWindows())
            urls.append(new QUrl(
                    "https://windows-package-manager.googlecode.com/hg/repository/Rep64.xml"));
        Repository::setRepositoryURLs(urls);
    }
    qDeleteAll(urls);
    urls.clear();

    //this->ui->formLayout_2->setSizeConstraint(QLayout::SetDefaultConstraint);

    this->ui->tabWidget->setTabText(0, "Software");

    this->on_tableWidget_itemSelectionChanged();
    this->ui->tableWidget->setColumnCount(6);
    this->ui->tableWidget->setColumnWidth(0, 40);
    this->ui->tableWidget->setColumnWidth(1, 150);
    this->ui->tableWidget->setColumnWidth(2, 300);
    this->ui->tableWidget->setIconSize(QSize(32, 32));
    this->ui->tableWidget->sortItems(1);
    this->ui->tableWidget->addAction(this->ui->actionInstall);
    this->ui->tableWidget->addAction(this->ui->actionUninstall);
    this->ui->tableWidget->addAction(this->ui->actionUpdate);
    this->ui->tableWidget->addAction(this->ui->actionShow_Details);
    this->ui->tableWidget->addAction(this->ui->actionGotoPackageURL);
    this->ui->tableWidget->addAction(this->ui->actionTest_Download_Site);

    connect(&this->fileLoader, SIGNAL(downloaded(const FileLoaderItem&)), this,
            SLOT(iconDownloaded(const FileLoaderItem&)),
            Qt::QueuedConnection);
    this->fileLoader.start(QThread::LowestPriority);
}

void MainWindow::iconDownloaded(const FileLoaderItem& it)
{
    if (it.f) {
        // qDebug() << "MainWindow::iconDownloaded.2 " << it.url;
        QPixmap pm(it.f->fileName());
        delete it.f;
        if (!pm.isNull()) {
            QIcon icon(pm);
            icon.detach();
            this->icons.insert(it.url, icon);
            updateIcons();
        }
    }
}

void MainWindow::prepare()
{
    QTimer *pTimer = new QTimer(this);
    pTimer->setSingleShot(true);
    connect(pTimer, SIGNAL(timeout()), this, SLOT(onShow()));

    pTimer->start(0);
}

MainWindow::~MainWindow()
{
    this->fileLoader.terminated = 1;
    if (!this->fileLoader.wait(1000))
        this->fileLoader.terminate();
    delete ui;
}

bool MainWindow::waitFor(Job* job, const QString& title)
{
    ProgressDialog* pd;
    pd = new ProgressDialog(this, job, title);
    pd->setModal(true);

    defaultPasswordWindow = pd->winId();
    // qDebug() << "MainWindow::waitFor.1";

    pd->exec();
    delete pd;
    pd = 0;

    // qDebug() << "MainWindow::waitFor.2";
    if (job->isCancelled())
        return false;
    else if (!job->getErrorMessage().isEmpty()) {
        QString first = job->getErrorMessage().trimmed();
        int ind = first.indexOf("\n");
        if (ind >= 0)
            first = first.left(ind);
        ind = first.indexOf("\r");
        if (ind >= 0)
            first = first.left(ind);

        QMessageBox mb(this);
        mb.setWindowTitle("Error");
        mb.setText(QString("%1: %2").
                   arg(job->getHint()).
                   arg(first));
        mb.setIcon(QMessageBox::Critical);
        mb.setStandardButtons(QMessageBox::Ok);
        mb.setDefaultButton(QMessageBox::Ok);
        mb.setDetailedText(job->getErrorMessage());
        mb.exec();

        return false;
    } else {
        return true;
    }
}

void MainWindow::onShow()
{
    recognizeAndLoadRepositories();
}

void MainWindow::selectPackageVersion(PackageVersion* pv)
{
    for (int i = 0; i < this->ui->tableWidget->rowCount(); i++) {
        const QVariant v = this->ui->tableWidget->item(i, 0)->
                data(Qt::UserRole);
        PackageVersion* f = (PackageVersion *) v.value<void*>();
        if (f == pv) {
            this->ui->tableWidget->selectRow(i);
            break;
        }
    }
}

PackageVersion* MainWindow::getSelectedPackageVersion()
{
    QWidget* w = this->ui->tabWidget->widget(this->ui->tabWidget->
            currentIndex());
    PackageVersionForm* pvf = dynamic_cast<PackageVersionForm*>(w);
    if (pvf) {
        return pvf->pv;
    } else if (w == this->ui->tab){
        QList<QTableWidgetItem*> sel = this->ui->tableWidget->selectedItems();
        if (sel.count() > 0) {
            const QVariant v = sel.at(0)->data(Qt::UserRole);
            PackageVersion* pv = (PackageVersion *) v.value<void*>();
            return pv;
        }
        return 0;
    } else {
        return 0;
    }
}

void MainWindow::fillList()
{
    // qDebug() << "MainWindow::fillList";
    QTableWidget* t = this->ui->tableWidget;

    t->clearContents();
    t->setSortingEnabled(false);

    Repository* r = Repository::getDefault();

    t->setColumnCount(6);

    QTableWidgetItem *newItem = new QTableWidgetItem("Icon");
    t->setHorizontalHeaderItem(0, newItem);

    newItem = new QTableWidgetItem("Title");
    t->setHorizontalHeaderItem(1, newItem);

    newItem = new QTableWidgetItem("Description");
    t->setHorizontalHeaderItem(2, newItem);

    newItem = new QTableWidgetItem("Version");
    t->setHorizontalHeaderItem(3, newItem);

    newItem = new QTableWidgetItem("Status");
    t->setHorizontalHeaderItem(4, newItem);

    newItem = new QTableWidgetItem("License");
    t->setHorizontalHeaderItem(5, newItem);

    int statusFilter = this->ui->comboBoxStatus->currentIndex();
    QStringList textFilter =
            this->ui->lineEditText->text().toLower().simplified().split(" ");

    t->setRowCount(r->packageVersions.count());

    int n = 0;
    for (int i = 0; i < r->packageVersions.count(); i++) {
        PackageVersion* pv = r->packageVersions.at(i);

        // filter by text
        if (textFilter.count() > 0) {
            QString fullText = pv->getFullText();
            bool b = true;
            for (int i = 0; i < textFilter.count(); i++) {
                if (fullText.indexOf(textFilter.at(i)) < 0) {
                    b = false;
                    break;
                }
            }
            if (!b)
                continue;
        }

        bool installed = pv->installed();
        bool updateEnabled = isUpdateEnabled(pv);
        PackageVersion* newest = r->findNewestPackageVersion(pv->package);
        bool statusOK;
        switch (statusFilter) {
            case 0:
                // all
                statusOK = true;
                break;
            case 1:
                // not installed
                statusOK = !installed;
                break;
            case 2:
                // installed
                statusOK = installed;
                break;
            case 3:
                // installed, updateable
                statusOK = installed && updateEnabled;
                break;
            case 4:
                // newest or installed
                statusOK = installed || pv == newest;
                break;
            default:
                statusOK = true;
                break;
        }
        if (!statusOK)
            continue;

        Package* p = r->findPackage(pv->package);

        newItem = new QTableWidgetItem("");
        newItem->setData(Qt::UserRole, qVariantFromValue((void*) pv));
        if (p) {
            if (!p->icon.isEmpty() && this->icons.contains(p->icon)) {
                QIcon icon = this->icons[p->icon];
                newItem->setIcon(icon);
            } else {
                newItem->setIcon(MainWindow::genericAppIcon);
            }
        } else {
            newItem->setIcon(MainWindow::genericAppIcon);
        }
        t->setItem(n, 0, newItem);

        QString packageTitle;
        if (p)
            packageTitle = p->title;
        else
            packageTitle = pv->package;
        newItem = new QTableWidgetItem(packageTitle);
        newItem->setStatusTip(pv->download.toString() + " " + pv->package +
                " " + pv->sha1);
        newItem->setData(Qt::UserRole, qVariantFromValue((void*) pv));
        t->setItem(n, 1, newItem);

        QString desc;
        if (p)
            desc = p->description;
        newItem = new QTableWidgetItem(desc);
        newItem->setData(Qt::UserRole, qVariantFromValue((void*) pv));
        t->setItem(n, 2, newItem);

        newItem = new QTableWidgetItem(pv->version.getVersionString());
        newItem->setData(Qt::UserRole, qVariantFromValue((void*) pv));
        t->setItem(n, 3, newItem);

        newItem = new QTableWidgetItem("");
        QString status;
        if (installed) {
            if (pv->isExternal())
                status = "installed externally";
            else
                status = "installed";
        }
        if (installed && pv != newest) {
            newItem->setBackgroundColor(QColor(255, 0xc7, 0xc7));
            if (updateEnabled)
                status += ", updateable";
            else
                status += ", obsolete";
        }
        newItem->setText(status);
        newItem->setData(Qt::UserRole, qVariantFromValue((void*) pv));
        t->setItem(n, 4, newItem);

        newItem = new QTableWidgetItem("");
        QString licenseTitle;
        if (p) {
            License* lic = r->findLicense(p->license);
            if (lic)
                licenseTitle = lic->title;
        }
        newItem->setText(licenseTitle);
        newItem->setData(Qt::UserRole, qVariantFromValue((void*) pv));
        t->setItem(n, 5, newItem);

        n++;
    }
    t->setRowCount(n);
    t->setSortingEnabled(true);
    // qDebug() << "MainWindow::fillList.2";
}

void MainWindow::process(QList<InstallOperation*> &install)
{
    // reoder the operations if a package is updated. In this case it is better
    // to uninstall the old first and then install the new one.
    if (install.size() == 2) {
        InstallOperation* first = install.at(0);
        InstallOperation* second = install.at(1);
        if (first->packageVersion->package == second->packageVersion->package &&
                first->install && !second->install) {
            install.insert(0, second);
            install.removeAt(2);
        }
    }

    PackageVersion* sel = getSelectedPackageVersion();

    QStringList locked = WPMUtils::getProcessFiles();
    QStringList lockedUninstall;
    for (int j = 0; j < install.size(); j++) {
        InstallOperation* op = install.at(j);
        if (!op->install) {
            PackageVersion* pv = op->packageVersion;
            QString path = pv->getPath();
            for (int i = 0; i < locked.size(); i++) {
                if (WPMUtils::isUnder(locked.at(i), path)) {
                    lockedUninstall.append(locked.at(i));
                }
            }
        }
    }

    if (lockedUninstall.size() > 0) {
        QString locked_ = lockedUninstall.join(", \n");
        QString msg("The package(s) cannot be uninstalled because "
                "the following files are in use "
                "(please close the corresponding applications): "
                "%1");
        QMessageBox::critical(this,
                "Uninstall", msg.arg(locked_));
        return;
    }

    for (int i = 0; i < install.count(); i++) {
        InstallOperation* op = install.at(i);
        if (!op->install) {
            PackageVersion* pv = op->packageVersion;
            if (pv->isDirectoryLocked()) {
                QString msg("The package %1 cannot be uninstalled because "
                        "some files or directories under %2 are in use.");
                QMessageBox::critical(this,
                        "Uninstall", msg.arg(pv->toString()).
                        arg(pv->getPath()));
                return;
            }
        }
    }

    QString names;
    for (int i = 0; i < install.count(); i++) {
        InstallOperation* op = install.at(i);
        if (!op->install) {
            PackageVersion* pv = op->packageVersion;
            if (!names.isEmpty())
                names.append(", ");
            names.append(pv->toString());
        }
    }
    QString installNames;
    for (int i = 0; i < install.count(); i++) {
        InstallOperation* op = install.at(i);
        if (op->install) {
            PackageVersion* pv = op->packageVersion;
            if (!installNames.isEmpty())
                installNames.append(", ");
            installNames.append(pv->toString());
        }
    }

    int installCount = 0, uninstallCount = 0;
    for (int i = 0; i < install.count(); i++) {
        InstallOperation* op = install.at(i);
        if (op->install)
            installCount++;
        else
            uninstallCount++;
    }

    bool b;
    QString msg;
    if (installCount == 1 && uninstallCount == 0) {
        b = true;
    } else if (installCount == 0 && uninstallCount == 1) {
        msg = QString("The package %1 will be uninstalled. "
                "The corresponding directory %2 "
                "will be completely deleted. "
                "There is no way to restore the files.").
                arg(install.at(0)->packageVersion->toString()).
                arg(install.at(0)->packageVersion->getPath());
        b = UIUtils::confirm(this, "Uninstall", msg);
    } else if (installCount > 0 && uninstallCount == 0) {
        msg = QString("%1 package(s) will be installed: %2").
                arg(installCount).arg(installNames);
        b = UIUtils::confirm(this, "Install", msg);
    } else if (installCount == 0 && uninstallCount > 0) {
        msg = QString("%1 package(s) will be uninstalled: %2. "
                "The corresponding directories "
                "will be completely deleted. "
                "There is no way to restore the files.").
                arg(uninstallCount).arg(names);
        b = UIUtils::confirm(this, "Uninstall", msg);
    } else {
        msg = QString("%1 package(s) will be uninstalled: %2 ("
                "the corresponding directories "
                "will be completely deleted; "
                "there is no way to restore the files) "
                "and %3 package(s) will be installed: %4.").arg(uninstallCount).
                arg(names).
                arg(installCount).
                arg(installNames);
        b = UIUtils::confirm(this, "Install/Uninstall", msg);
    }

    if (b) {
        Job* job = new Job();
        InstallThread* it = new InstallThread(0, 1, job);
        it->install = install;
        it->start();
        it->setPriority(QThread::LowestPriority);

        waitFor(job, "Install/Uninstall");
        it->wait();
        delete it;
        delete job;

        fillList();
        updateStatusInDetailTabs();
        selectPackageVersion(sel);
    }
}

void MainWindow::changeEvent(QEvent *e)
{
    QMainWindow::changeEvent(e);
    switch (e->type()) {
    case QEvent::LanguageChange:
        ui->retranslateUi(this);
        break;
    default:
        break;
    }
}

void MainWindow::on_actionExit_triggered()
{
    this->close();
}

void MainWindow::on_actionUninstall_activated()
{
    PackageVersion* pv = getSelectedPackageVersion();
    QList<InstallOperation*> ops;
    QList<PackageVersion*> installed = Repository::getDefault()->getInstalled();
    QString err = pv->planUninstallation(installed, ops);
    if (err.isEmpty())
        process(ops);
    else
        QMessageBox::critical(this,
                "Uninstall", err, QMessageBox::Ok);
}

bool MainWindow::isUpdateEnabled(PackageVersion* pv)
{
    if (pv && !pv->isExternal()) {
        Repository* r = Repository::getDefault();
        PackageVersion* newest = r->findNewestPackageVersion(pv->package);
        PackageVersion* newesti = r->findNewestInstalledPackageVersion(
                pv->package);
        return newest != 0 && newesti != 0 &&
                !newesti->isExternal() &&
                newest->version.compare(newesti->version) > 0;
    } else {
        return false;
    }
}

void MainWindow::updateActions()
{
    PackageVersion* pv = getSelectedPackageVersion();
    this->ui->actionInstall->setEnabled(pv && !pv->installed());
    this->ui->actionUninstall->setEnabled(pv && pv->installed() &&
            !pv->isExternal());
    this->ui->actionCompute_SHA1->setEnabled(pv && pv->download.isValid());

    // "Update"
    this->ui->actionUpdate->setEnabled(isUpdateEnabled(pv));

    // enable "Go To Package Page"
    Package* p;
    if (pv)
        p = Repository::getDefault()->findPackage(pv->package);
    else
        p = 0;
    this->ui->actionGotoPackageURL->setEnabled(pv && p &&
            QUrl(p->url).isValid());

    this->ui->actionTest_Download_Site->setEnabled(pv && p &&
            QUrl(p->url).isValid());

    this->ui->actionShow_Details->setEnabled(pv);
}

void MainWindow::on_tableWidget_itemSelectionChanged()
{
    updateActions();
}

void MainWindow::closeDetailTabs()
{
    for (int i = 0; i < this->ui->tabWidget->count(); ) {
        QWidget* w = this->ui->tabWidget->widget(i);
        if (w != this->ui->tab) {
            this->ui->tabWidget->removeTab(i);
        } else {
            i++;
        }
    }
}

void MainWindow::recognizeAndLoadRepositories()
{
    QTableWidget* t = this->ui->tableWidget;
    t->clearContents();
    t->setRowCount(0);

    Job* job = new Job();
    InstallThread* it = new InstallThread(0, 3, job);
    it->start();
    it->setPriority(QThread::LowestPriority);

    QString title("Initializing");
    waitFor(job, title);
    it->wait();
    delete it;

    fillList();
    delete job;

    Repository* r = Repository::getDefault();
    for (int i = 0; i < r->packages.count(); i++) {
        Package* p = r->packages.at(i);
        if (!p->icon.isEmpty()) {
            FileLoaderItem it;
            it.url = p->icon;
            // qDebug() << "MainWindow::loadRepository " << it.url;
            this->fileLoader.work.append(it);
        }
    }
    // qDebug() << "MainWindow::loadRepository";
}

void MainWindow::on_actionInstall_activated()
{
    PackageVersion* pv = getSelectedPackageVersion();

    QList<PackageVersion*> r;
    QList<InstallOperation*> ops;
    QList<PackageVersion*> installed =
            Repository::getDefault()->getInstalled();
    QString err = pv->planInstallation(installed, ops);
    if (err.isEmpty())
        process(ops);
    else
        QMessageBox::critical(this,
                "Install", err, QMessageBox::Ok);
}

void MainWindow::on_actionGotoPackageURL_triggered()
{
    PackageVersion* pv = getSelectedPackageVersion();
    if (pv) {
        Package* p = Repository::getDefault()->findPackage(pv->package);
        if (p) {
            QUrl url(p->url);
            if (url.isValid()) {
                QDesktopServices::openUrl(url);
            }
        }
    }
}

void MainWindow::on_comboBoxStatus_currentIndexChanged(int index)
{
    this->fillList();
}

void MainWindow::on_lineEditText_textChanged(QString )
{
    this->fillList();
}

void MainWindow::on_actionSettings_triggered()
{
    SettingsDialog d;

    QList<QUrl*> urls = Repository::getRepositoryURLs();
    QStringList list;
    for (int i = 0; i < urls.count(); i++) {
        list.append(urls.at(i)->toString());
    }
    d.setRepositoryURLs(list);
    qDeleteAll(urls);
    urls.clear();

    d.setInstallationDirectory(WPMUtils::getInstallationDirectory());

    if (d.exec() == QDialog::Accepted) {
        list = d.getRepositoryURLs();
        if (list.count() == 0) {
            QMessageBox::critical(this,
                    "Error", "No repositories defined", QMessageBox::Ok);
        } else if (d.getInstallationDirectory().isEmpty()) {
            QMessageBox::critical(this,
                    "Error", "The installation directory cannot be empty",
                    QMessageBox::Ok);
        } else if (!QDir(d.getInstallationDirectory()).exists()) {
            QMessageBox::critical(this,
                    "Error", "The installation directory does not exist",
                    QMessageBox::Ok);
        } else {
            QString err;
            for (int i = 0; i < list.count(); i++) {
                QUrl* url = new QUrl(list.at(i));
                urls.append(url);
                if (!url->isValid()) {
                    err = QString("%1 is not a valid repository address").arg(
                            list.at(i));
                    break;
                }
            }
            if (err.isEmpty()) {
                WPMUtils::setInstallationDirectory(d.getInstallationDirectory());
                Repository::setRepositoryURLs(urls);
                closeDetailTabs();
                recognizeAndLoadRepositories();
            } else {
                QMessageBox::critical(this,
                        "Error", err, QMessageBox::Ok);
            }
            qDeleteAll(urls);
            urls.clear();
        }
    }
}

void MainWindow::on_actionUpdate_triggered()
{
    PackageVersion* pv = getSelectedPackageVersion();
    Repository* r = Repository::getDefault();
    PackageVersion* newest = r->findNewestPackageVersion(pv->package);
    PackageVersion* newesti = r->findNewestInstalledPackageVersion(pv->package);

    QList<InstallOperation*> ops;
    QList<PackageVersion*> installed =
            Repository::getDefault()->getInstalled();
    QString err = newest->planInstallation(installed, ops);
    if (err.isEmpty())
        err = newesti->planUninstallation(installed, ops);

    if (err.isEmpty()) {
        InstallOperation::simplify(ops);
        process(ops);
    } else
        QMessageBox::critical(this,
                "Uninstall", err, QMessageBox::Ok);
}

void MainWindow::on_actionTest_Download_Site_triggered()
{
    PackageVersion* pv = getSelectedPackageVersion();
    if (pv) {
        QString s = "http://www.urlvoid.com/scan/" + pv->download.host();
        QUrl url(s);
        if (url.isValid()) {
            QDesktopServices::openUrl(url);
        }
    }
}

void MainWindow::on_actionCompute_SHA1_triggered()
{
    PackageVersion* pv = getSelectedPackageVersion();
    Job* job = new Job();
    InstallThread* it = new InstallThread(pv, 5, job);
    it->start();
    it->setPriority(QThread::LowestPriority);

    waitFor(job, "Compute SHA1");
    it->wait();

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        bool ok;
        QInputDialog::getText(this, "SHA1",
                QString("SHA1 for %1:").arg(pv->toString()), QLineEdit::Normal,
                it->sha1, &ok);
    }

    delete it;
    delete job;
}

void MainWindow::on_actionAbout_triggered()
{
    QMessageBox::about(this, "About",
            QString("<html><body>Npackd %1 - software package manager for Windows (R)\n"
            "<a href='http://code.google.com/p/windows-package-manager'>"
            "http://code.google.com/p/windows-package-manager</a></body></html>").
            arg(WPMUtils::NPACKD_VERSION));
}

void MainWindow::on_actionTest_Repositories_triggered()
{
    QString msg = QString("All packages will be uninstalled. "
            "The corresponding directories will be deleted. "
            "There is no way to restore the files. "
            "Are you sure?");
    QMessageBox::StandardButton b = QMessageBox::warning(this,
            "Uninstall", msg, QMessageBox::Yes | QMessageBox::No);

    if (b == QMessageBox::Yes) {
        QString fn = QFileDialog::getSaveFileName(this,
                tr("Save Log File"),
                "", tr("Log Files (*.log)"));
        if (!fn.isEmpty()) {
            Job* job = new Job();
            InstallThread* it = new InstallThread(0, 6, job);
            it->logFile = fn;
            it->start();
            it->setPriority(QThread::LowestPriority);

            waitFor(job, "Test Repositories");
            it->wait();
            delete it;
            delete job;

            fillList();
        }
    }
}

void MainWindow::on_tableWidget_doubleClicked(QModelIndex index)
{
    showDetails();
}

void MainWindow::on_tabWidget_tabCloseRequested(int index)
{
    QWidget* w = this->ui->tabWidget->widget(index);
    if (w != this->ui->tab) {
        this->ui->tabWidget->removeTab(index);
    }
}

void MainWindow::on_tabWidget_currentChanged(int index)
{
    updateActions();
}

void MainWindow::on_actionList_Installed_MSI_Products_triggered()
{
    QStringList sl = WPMUtils::findInstalledMSIProductNames();

    QTextEdit* te = new QTextEdit(this->ui->tabWidget);
    te->setReadOnly(true);
    te->setText(sl.join("\n"));
    this->ui->tabWidget->addTab(te, "Installed MSI Products");
    this->ui->tabWidget->setCurrentIndex(this->ui->tabWidget->count() - 1);
}

void MainWindow::on_actionDownload_All_Files_triggered()
{
    QString fn = QFileDialog::getExistingDirectory(this,
            "Download All Files", ".", QFileDialog::ShowDirsOnly);
    if (!fn.isEmpty()) {
        QDir d(fn);
        QFileInfoList entries = d.entryInfoList(
                QDir::NoDotAndDotDot |
                QDir::AllEntries);
        if (entries.size() != 0) {
            QMessageBox::critical(this, "Error",
                    "The chosen directory is not empty");
        } else {
            Job* job = new Job();
            InstallThread* it = new InstallThread(0, 7, job);
            it->logFile = fn;
            it->start();
            it->setPriority(QThread::LowestPriority);

            waitFor(job, "Download All Files");
            it->wait();
            delete it;
            delete job;
        }
    }
}


void MainWindow::on_actionShow_Details_triggered()
{
    showDetails();
}

void MainWindow::on_actionScan_Hard_Drives_triggered()
{
    Job* job = new Job();
    InstallThread* it = new InstallThread(0, 8, job);
    it->start();
    it->setPriority(QThread::LowestPriority);

    waitFor(job, "Scan Hard Drives");
    it->wait();
    delete it;
    delete job;

    fillList();
}
