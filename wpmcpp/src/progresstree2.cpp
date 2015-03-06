#include <math.h>

#include <QList>
#include <QTreeWidgetItem>
#include <QStringList>
#include <QString>
#include <QProgressBar>
#include <QPushButton>
#include <QMessageBox>
#include <QHeaderView>
#include <QDebug>

#include "progresstree2.h"
#include "job.h"
#include "wpmutils.h"
#include "mainwindow.h"

class CancelPushButton: public QPushButton
{
public:
    QTreeWidgetItem* item;

    CancelPushButton(QWidget *parent): QPushButton(parent) {}
};

ProgressTree2::ProgressTree2(QWidget *parent) :
    QTreeWidget(parent)
{
    this->autoExpandNodes = true;
    setColumnCount(5);

    timer = new QTimer(this);
    connect(timer, SIGNAL(timeout()), this, SLOT(timerTimeout()));
    timer->start(1000);

    this->monitoredJobLastChanged = 0;

    setColumnWidth(0, 500);
    setColumnWidth(1, 100);
    setColumnWidth(2, 100);
    setColumnWidth(3, 200);
    setColumnWidth(4, 100);
    header()->setStretchLastSection(false);

    QStringList hls;
    hls.append(QObject::tr("Task / Step"));
    hls.append(QObject::tr("Elapsed time"));
    hls.append(QObject::tr("Remaining time"));
    hls.append(QObject::tr("Progress"));
    hls.append(QObject::tr(""));
    setHeaderLabels(hls);
}

void ProgressTree2::timerTimeout()
{

}

QTreeWidgetItem* ProgressTree2::findItem(Job* job, bool create)
{
    QList<Job*> path;
    Job* v = job;
    while (v) {
        path.insert(0, v);
        v = v->parentJob;
    }

    QTreeWidgetItem* c = 0;
    for (int i = 0; i < path.count(); i++) {
        Job* toFind = path.at(i);
        QTreeWidgetItem* found = 0;
        if (i == 0) {
            for (int j = 0; j < this->topLevelItemCount(); j++) {
                QTreeWidgetItem* item = this->topLevelItem(j);
                Job* v = getJob(*item);
                if (v == toFind) {
                    found = item;
                    break;
                }
            }
        } else {
            for (int j = 0; j < c->childCount(); j++) {
                QTreeWidgetItem* item = c->child(j);
                Job* v = getJob(*item);
                if (v == toFind) {
                    found = item;
                    break;
                }
            }
        }
        if (found)
            c = found;
        else {
            if (create) {
                if (!toFind->parentJob)
                    c = addJob(toFind);
                else {
                    QTreeWidgetItem* subItem = new QTreeWidgetItem(c);
                    fillItem(subItem, toFind);
                    if (autoExpandNodes)
                        c->setExpanded(true);
                    c = subItem;
                }
            } else {
                c = 0;
                break;
            }
        }
    }

    return c;
}

void ProgressTree2::fillItem(QTreeWidgetItem* item,
        Job* job)
{
    item->setText(0, job->getTitle().isEmpty() ?
            "-" : job->getTitle());

    QProgressBar* pb = new QProgressBar(this);
    pb->setMaximum(10000);
    setItemWidget(item, 3, pb);

    CancelPushButton* cancel = new CancelPushButton(this);
    cancel->setText(QObject::tr("Cancel"));
    cancel->item = item;
    connect(cancel, SIGNAL(clicked()), this,
            SLOT(cancelClicked()));
    setItemWidget(item, 4, cancel);

    item->setData(0, Qt::UserRole, qVariantFromValue((void*) job));
}

QTreeWidgetItem* ProgressTree2::addJob(Job* job)
{
    QTreeWidgetItem* item = new QTreeWidgetItem(this);
    fillItem(item, job);

    connect(job, SIGNAL(subJobCreated(Job*)), this,
            SLOT(subJobCreated(Job*)),
            Qt::QueuedConnection);

    connect(job, SIGNAL(changed(const JobState&)), this,
            SLOT(monitoredJobChanged(const JobState&)),
            Qt::QueuedConnection);

    addTopLevelItem(item);

    return item;
}

void ProgressTree2::setNarrowColumns()
{
    setColumnWidth(0, 110);
    setColumnWidth(1, 60);
    setColumnWidth(2, 60);
    setColumnWidth(3, 100);
    setColumnWidth(4, 70);
}

void ProgressTree2::subJobCreated(Job* sub)
{
    findItem(sub, true);
}

void ProgressTree2::cancelClicked()
{
    CancelPushButton* pb = (CancelPushButton*) QObject::sender();
    pb->setEnabled(false);
    Job* job = getJob(*pb->item);
    job->cancel();
}

void ProgressTree2::monitoredJobChanged(const JobState& state)
{
    time_t now;
    time(&now);

    monitoredJobLastChanged = now;

    QTreeWidgetItem* item = findItem(state.job, true);
    if (item)
        updateItem(item, state);

    if (state.completed) {
        if (item) {
            setItemWidget(item, 4, 0);
        }

        Job* job = state.job;

        if (!job->parentJob && !job->getErrorMessage().isEmpty() &&
                !MainWindow::getInstance()) {
            QString jobTitle = job->getTitle();
            QString title = QObject::tr("Error") + ": " + jobTitle +
                        ": " + WPMUtils::getFirstLine(job->getErrorMessage());
            QString msg = job->getFullTitle() + "\n" + job->getErrorMessage();
            QMessageBox mb;
            mb.setWindowTitle(title);
            mb.setText(msg);
            mb.setIcon(QMessageBox::Warning);
            mb.setStandardButtons(QMessageBox::Ok);
            mb.setDefaultButton(QMessageBox::Ok);
            mb.setDetailedText(msg);
            mb.exec();
        }

        if (!job->parentJob) {
            delete item;
        }
    }
}

void ProgressTree2::updateItem(QTreeWidgetItem* item, const JobState& s)
{
    item->setText(0, s.title);

    time_t now;
    time(&now);

    if (s.started != 0) {
        time_t diff = difftime(now, s.started);

        int sec = diff % 60;
        diff /= 60;
        int min = diff % 60;
        diff /= 60;
        int h = diff;

        QTime e(h, min, sec);
        item->setText(1, e.toString());

        if (!s.completed) {
            diff = difftime(now, s.started);
            diff = lround(diff * (1 / s.progress - 1));
            sec = diff % 60;
            diff /= 60;
            min = diff % 60;
            diff /= 60;
            h = diff;

            QTime r(h, min, sec);
            item->setText(2, r.toString());
        } else {
            item->setText(2, "");
        }
    } else {
        item->setText(1, "");
        item->setText(2, "");
    }

    QProgressBar* pb = (QProgressBar*) itemWidget(item, 3);
    QPushButton* b = (QPushButton*) itemWidget(item, 4);
    if (s.completed) {
        pb->setValue(10000);
        b->setEnabled(false);
    } else {
        pb->setValue(lround(s.progress * 10000));
        b->setEnabled(!s.cancelRequested);
    }
}

Job* ProgressTree2::getJob(const QTreeWidgetItem& item)
{
    const QVariant v = item.data(0, Qt::UserRole);
    Job* f = (Job*) v.value<void*>();
    return f;
}
