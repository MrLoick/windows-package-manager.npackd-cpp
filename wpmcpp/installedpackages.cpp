#include "installedpackages.h"

#include <windows.h>
#include <QDebug>
#include <msi.h>

#include "windowsregistry.h"
#include "package.h"
#include "version.h"
#include "packageversion.h"
#include "repository.h"
#include "wpmutils.h"
#include "controlpanelthirdpartypm.h"
#include "msithirdpartypm.h"
#include "wellknownprogramsthirdpartypm.h"

InstalledPackages InstalledPackages::def;

InstalledPackages* InstalledPackages::getDefault()
{
    return &def;
}

InstalledPackages::InstalledPackages()
{
}

InstalledPackageVersion* InstalledPackages::find(const QString& package,
        const Version& version) const
{
    return this->data.value(PackageVersion::getStringId(package, version));
}

void InstalledPackages::detect3rdParty(AbstractThirdPartyPM *pm)
{
    Repository rep;
    QList<InstalledPackageVersion*> installed;
    pm->scan(&installed, &rep);
    AbstractRepository* r = AbstractRepository::getDefault_();
    for (int i = 0; i < rep.packages.count(); i++) {
        Package* p = rep.packages.at(i);
        Package* fp = r->findPackage_(p->name);
        if (fp)
            delete fp;
        else
            r->savePackage(p); // TODO: handle error
    }
    for (int i = 0; i < rep.packageVersions.count(); i++) {
        PackageVersion* pv = rep.packageVersions.at(i);
        PackageVersion* fpv = r->findPackageVersion_(pv->package, pv->version);
        if (fpv)
            delete fpv;
        else
            r->savePackageVersion(pv); // TODO: handle error

        Package* p = r->findPackage_(pv->package);
        if (!p) {
            p = new Package(pv->package, pv->package);
            r->savePackage(p);
        }
        delete p;
    }

    QDir d;
    for (int i = 0; i < installed.count(); i++) {
        InstalledPackageVersion* ipv = installed.at(i);
        PackageVersion* pv = r->findPackageVersion_(ipv->package, ipv->version);
        if (pv) {
            QString path = getPath(ipv->package, ipv->version);
            if (path.isEmpty()) {
                PackageVersionFile* u = pv->findFile(".Npackd\\Uninstall.bat");
                if (u) {
                    path = ipv->directory;
                    if (path.isEmpty()) {
                        Package* p = r->findPackage_(ipv->package);
                        path = WPMUtils::getInstallationDirectory() +
                                "\\NpackdDetected\\" +
                        WPMUtils::makeValidFilename(p->title, '_');
                        if (d.exists(path)) {
                            path = WPMUtils::findNonExistingFile(path + "-" +
                                    ipv->version.getVersionString() + "%1");
                        }
                        d.mkpath(path);
                        delete p;
                    }
                    if (d.exists(path)) {
                        if (d.mkpath(path + "\\.Npackd")) {
                            QFile file(path + "\\.Npackd\\Uninstall.bat");
                            if (file.open(QIODevice::WriteOnly |
                                    QIODevice::Truncate)) {
                                QTextStream stream(&file);
                                stream.setCodec("UTF-8");
                                stream << u->content;
                                file.close();

                                //qDebug() << "InstalledPackages::detectOneControlPanelProgram "
                                //        "setting path for " << pv->toString() << " to" << dir;
                                setPackageVersionPath(ipv->package,
                                        ipv->version, path);
                            }
                        }
                    }
                }
            }
            delete pv;
        }
    }
    qDeleteAll(installed);

    /* TODO:
     *        if (!dir.isEmpty()) {
            dir = WPMUtils::normalizePath(dir);
            if (WPMUtils::isUnderOrEquals(dir, packagePaths))
                dir = "";
        }
*/
    /* TODO:
    // remove uninstalled packages
    QMapIterator<QString, InstalledPackageVersion*> i(data);
    while (i.hasNext()) {
        i.next();
        InstalledPackageVersion* ipv = i.value();
        if (ipv->detectionInfo.indexOf("control-panel:") == 0 &&
                ipv->installed() &&
                !foundDetectionInfos.contains(ipv->detectionInfo)) {
            qDebug() << "control-panel package removed: " << ipv->package;
            ipv->setPath("");
        }
    }
    */
}

InstalledPackageVersion* InstalledPackages::findOrCreate(const QString& package,
        const Version& version)
{
    QString key = PackageVersion::getStringId(package, version);
    InstalledPackageVersion* r = this->data.value(key);
    if (!r) {
        r = new InstalledPackageVersion(package, version, "");
        this->data.insert(key, r);

        // qDebug() << "InstalledPackages::findOrCreate " << package;
        // TODO: error is not handled
        saveToRegistry(r);
        fireStatusChanged(package, version);
    }
    return r;
}

QString InstalledPackages::setPackageVersionPath(const QString& package,
        const Version& version,
        const QString& directory)
{
    QString err;

    InstalledPackageVersion* ipv = this->find(package, version);
    if (!ipv) {
        ipv = new InstalledPackageVersion(package, version, directory);
        this->data.insert(package + "/" + version.getVersionString(), ipv);
        err = saveToRegistry(ipv);
        fireStatusChanged(package, version);
    } else {
        ipv->setPath(directory);
        err = saveToRegistry(ipv);
        fireStatusChanged(package, version);
    }

    return err;
}

void InstalledPackages::setPackageVersionPathIfNotInstalled(
        const QString& package,
        const Version& version,
        const QString& directory)
{
    InstalledPackageVersion* ipv = findOrCreate(package, version);
    if (!ipv->installed())
        ipv->setPath(directory);
}

QList<InstalledPackageVersion*> InstalledPackages::getAll() const
{
    QList<InstalledPackageVersion*> all = this->data.values();
    QList<InstalledPackageVersion*> r;
    for (int i = 0; i < all.count(); i++) {
        InstalledPackageVersion* ipv = all.at(i);
        if (ipv->installed())
            r.append(ipv->clone());
    }
    return r;
}

QStringList InstalledPackages::getAllInstalledPackagePaths() const
{
    QStringList r;
    QList<InstalledPackageVersion*> ipvs = this->data.values();
    for (int i = 0; i < ipvs.count(); i++) {
        InstalledPackageVersion* ipv = ipvs.at(i);
        if (ipv->installed())
            r.append(ipv->getDirectory());
    }
    return r;
}

void InstalledPackages::refresh(Job *job)
{

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        job->setHint("Detecting directories deleted externally");
        QList<InstalledPackageVersion*> ipvs = this->data.values();
        for (int i = 0; i < ipvs.count(); i++) {
            InstalledPackageVersion* ipv = ipvs.at(i);
            if (ipv->installed()) {
                QDir d(ipv->getDirectory());
                d.refresh();
                if (!d.exists()) {
                    ipv->directory = "";
                    // TODO: error message is not handled
                    saveToRegistry(ipv);
                    fireStatusChanged(ipv->package, ipv->version);
                }
            }
        }
        job->setProgress(0.2);
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        job->setHint("Reading registry package database");
        readRegistryDatabase();
        job->setProgress(0.5);
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        job->setHint("Detecting software");
        Job* d = job->newSubJob(0.2);

        /* TODO
        job->setHint("Updating NPACKD_CL");
        AbstractRepository* rep = AbstractRepository::getDefault_();
        rep->updateNpackdCLEnvVar();
        job->setProgress(1);
        */

        AbstractThirdPartyPM* pm = new WellKnownProgramsThirdPartyPM();
        detect3rdParty(pm);
        delete pm;

        // MSI package detection should happen before the detection for
        // control panel programs
        pm = new MSIThirdPartyPM();
        detect3rdParty(pm);
        delete pm;

        pm = new ControlPanelThirdPartyPM();
        detect3rdParty(pm);
        delete pm;

        delete d;
    }

    if (job->shouldProceed(
            "Clearing information about installed package versions in nested directories")) {
        clearPackagesInNestedDirectories();
        job->setProgress(1);
    }

    job->complete();
}

QString InstalledPackages::getPath(const QString &package,
        const Version &version) const
{
    QString r;
    InstalledPackageVersion* ipv = find(package, version);
    if (ipv)
        r = ipv->getDirectory();

    return r;
}

bool InstalledPackages::isInstalled(const QString &package,
        const Version &version) const
{
    InstalledPackageVersion* ipv = find(package, version);
    return ipv && ipv->installed();
}

void InstalledPackages::fireStatusChanged(const QString &package,
        const Version &version)
{
    emit statusChanged(package, version);
}

void InstalledPackages::clearPackagesInNestedDirectories() {
    /* TODO:
    QList<PackageVersion*> pvs = this->getInstalled();
    qSort(pvs.begin(), pvs.end(), packageVersionLessThan2);

    for (int j = 0; j < pvs.count(); j++) {
        PackageVersion* pv = pvs.at(j);
        if (pv->installed() && !WPMUtils::pathEquals(pv->getPath(),
                WPMUtils::getWindowsDir())) {
            for (int i = j + 1; i < pvs.count(); i++) {
                PackageVersion* pv2 = pvs.at(i);
                if (pv2->installed() && !WPMUtils::pathEquals(pv2->getPath(),
                        WPMUtils::getWindowsDir())) {
                    if (WPMUtils::isUnder(pv2->getPath(), pv->getPath()) ||
                            WPMUtils::pathEquals(pv2->getPath(), pv->getPath())) {
                        pv2->setPath("");
                    }
                }
            }
        }
    }
    */
}

void InstalledPackages::readRegistryDatabase()
{
    this->data.clear();

    WindowsRegistry machineWR(HKEY_LOCAL_MACHINE, false, KEY_READ);

    AbstractRepository* rep = AbstractRepository::getDefault_();

    QString err;
    WindowsRegistry packagesWR;
    err = packagesWR.open(machineWR,
            "SOFTWARE\\Npackd\\Npackd\\Packages", KEY_READ);
    if (err.isEmpty()) {
        QStringList entries = packagesWR.list(&err);
        for (int i = 0; i < entries.count(); ++i) {
            QString name = entries.at(i);
            int pos = name.lastIndexOf("-");
            if (pos > 0) {
                QString packageName = name.left(pos);
                if (Package::isValidName(packageName)) {
                    QString versionName = name.right(name.length() - pos - 1);
                    Version version;
                    if (version.setVersion(versionName)) {
                        rep->addPackageVersion(packageName, version);
                        InstalledPackageVersion* ipv =
                                loadFromRegistry(packageName, version);
                        if (ipv) {
                            this->data.insert(PackageVersion::getStringId(
                                    packageName, version), ipv);
                            fireStatusChanged(packageName, version);
                        }
                    }
                }
            }
        }
    }
}

InstalledPackageVersion* InstalledPackages::loadFromRegistry(
        const QString& package,
        const Version& version)
{
    InstalledPackageVersion* r = 0;

    // qDebug() << "InstalledPackageVersion::loadFromRegistry";

    WindowsRegistry entryWR;
    QString err = entryWR.open(HKEY_LOCAL_MACHINE,
            "SOFTWARE\\Npackd\\Npackd\\Packages\\" +
            package + "-" + version.getVersionString(),
            false, KEY_READ);
    if (!err.isEmpty())
        return r;

    QString p = entryWR.get("Path", &err).trimmed();
    if (!err.isEmpty())
        return r;

    QString dir;
    if (p.isEmpty())
        dir = "";
    else {
        QDir d(p);
        if (d.exists()) {
            dir = p;
        } else {
            dir = "";
        }
    }

    QString detectionInfo = entryWR.get("DetectionInfo", &err);

    r = new InstalledPackageVersion(package, version, dir);
    r->detectionInfo = detectionInfo;

    return r;
}

QString InstalledPackages::saveToRegistry(InstalledPackageVersion *ipv)
{
    WindowsRegistry machineWR(HKEY_LOCAL_MACHINE, false);
    QString r;
    QString keyName = "SOFTWARE\\Npackd\\Npackd\\Packages";
    QString pn = ipv->package + "-" + ipv->version.getVersionString();
    if (!ipv->directory.isEmpty()) {
        WindowsRegistry wr = machineWR.createSubKey(keyName + "\\" + pn, &r);
        if (r.isEmpty()) {
            r = wr.set("Path", ipv->directory);
            if (r.isEmpty())
                r = wr.set("DetectionInfo", ipv->detectionInfo);

            // for compatibility with Npackd 1.16 and earlier. They
            // see all package versions by default as "externally installed"
            if (r.isEmpty())
                r = wr.setDWORD("External", 0);
        }
    } else {
        // qDebug() << "deleting " << pn;
        WindowsRegistry packages;
        r = packages.open(machineWR, keyName, KEY_ALL_ACCESS);
        if (r.isEmpty()) {
            r = packages.remove(pn);
        }
    }
    //qDebug() << "InstalledPackageVersion::save " << pn << " " <<
    //        this->directory;
    return r;
}
