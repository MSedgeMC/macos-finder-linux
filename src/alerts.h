// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#pragma once
#include <QDialog>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QPlainTextEdit>
#include <QPainter>
#include <QPainterPath>
#include <QTextDocument>
#include <QPointer>
#include <KIO/JobUiDelegate>
#include <KIO/Global>
#include <KJob>

// Presentation only: KIO retains its native operation/conflict/auth/Undo
// interfaces. This opaque, rounded fallback does not sample a wallpaper or
// claim Apple's material renderer.
class FinderAlert final : public QDialog {
public:
    FinderAlert(const QString &title, const QString &body, const QString &details, QWidget *parent)
        : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint) {
        setObjectName(QStringLiteral("finderErrorAlert")); setWindowTitle(title);
        setWindowModality(parent ? Qt::WindowModal : Qt::ApplicationModal);
        setAttribute(Qt::WA_TranslucentBackground); setAttribute(Qt::WA_DeleteOnClose);
        setSizeGripEnabled(false); setAccessibleName(title);
        setProperty("errorDetails", details); setProperty("errorMessage", body);
        auto *layout = new QVBoxLayout(this); layout->setContentsMargins(16,20,16,16); layout->setSpacing(14);
        auto *text = new QWidget(this); auto *textLayout = new QVBoxLayout(text);
        textLayout->setContentsMargins(6,0,6,0); textLayout->setSpacing(10);
        auto *heading = new QLabel(title,text); heading->setObjectName("finderAlertTitle");
        auto *message = new QLabel(body,text); message->setObjectName("finderAlertMessage");
        for (auto *label : {heading,message}) {
            label->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Maximum); label->setTextFormat(Qt::PlainText); label->setWordWrap(true); label->setAlignment(Qt::AlignHCenter);
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            QFont f=label->font(); f.setPixelSize(13); if(label==heading)f.setWeight(QFont::Bold); label->setFont(f);
        }
        textLayout->addWidget(heading); textLayout->addWidget(message); layout->addWidget(text);
        if (!details.isEmpty() && details != body) {
            auto *toggle = new QToolButton(this); toggle->setText(tr("Show Details")); toggle->setObjectName("finderAlertDetailsToggle");
            toggle->setCheckable(true); toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); toggle->setArrowType(Qt::RightArrow);
            auto *native = new QPlainTextEdit(details,this); native->setObjectName("finderAlertDetails");
            native->setReadOnly(true); native->setFixedHeight(120); native->hide();
            layout->addWidget(toggle,0,Qt::AlignHCenter); layout->addWidget(native);
            connect(toggle,&QToolButton::toggled,this,[this,toggle,native](bool expanded) {
                native->setVisible(expanded);toggle->setText(expanded ? tr("Hide Details") : tr("Show Details"));
                toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);this->layout()->activate();adjustSize();
            });
        }
        auto *ok = new QPushButton(tr("OK"),this); ok->setObjectName("finderAlertOK"); ok->setMinimumHeight(28);
        ok->setDefault(true); ok->setAutoDefault(true); layout->addWidget(ok); connect(ok,&QPushButton::clicked,this,&QDialog::accept);
        const auto p=palette();
        setStyleSheet(QStringLiteral("#finderErrorAlert QLabel{background:transparent;color:%1;} #finderAlertOK{background:%2;color:%3;border:0;border-radius:14px;padding:4px 14px;} #finderAlertOK[finderKeyboardFocus=true]:focus{border:2px solid %4;} #finderAlertOK:pressed{background:%5;} #finderAlertDetails{background:%6;border:1px solid %7;border-radius:8px;} #finderAlertDetailsToggle{border:0;background:transparent;padding:2px;}")
            .arg(p.color(QPalette::Text).name(),p.color(QPalette::Highlight).name(),p.color(QPalette::HighlightedText).name(),
                 p.color(QPalette::Link).name(),p.color(QPalette::Highlight).darker(115).name(),p.color(QPalette::Base).name(),p.color(QPalette::Mid).name()));
        setFixedWidth(320); adjustSize(); ok->setFocus();
    }
    static QString plain(const QString &text) {
        if (!Qt::mightBeRichText(text)) return text;
        QTextDocument doc; doc.setHtml(text); return doc.toPlainText();
    }
    static void showError(QWidget *parent, const QString &title, const QString &body, const QString &details = {}) {
        const QString full=plain(body);
        // Native diagnostics can be very long. Keep the summary on screen;
        // the scrollable details retain the complete original message.
        const QString message=full.size()>350 ? full.left(350)+QStringLiteral("…") : full;
        const QString native=details.isEmpty() && message!=full ? full : plain(details);
        if (parent) for (auto *dialog : parent->findChildren<QDialog *>("finderErrorAlert"))
            if (auto *existing = dynamic_cast<FinderAlert *>(dialog); existing && existing->isVisible() && existing->windowTitle()==title && existing->property("errorDetails").toString()==native && existing->property("errorMessage").toString()==message) {existing->raise();return;}
        (new FinderAlert(title,message,native,parent))->open();
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen); p.setBrush(palette().color(QPalette::Window));
        p.drawRoundedRect(QRectF(rect()),26,26);
    }
};

class FinderJobUiDelegate : public KIO::JobUiDelegate {
public:
    explicit FinderJobUiDelegate(KJobUiDelegate::Flags flags, QWidget *window, const QList<QObject *> &interfaces = {})
        : KIO::JobUiDelegate(flags,window,interfaces), host(window) {}
    void showErrorMessage() override {
        auto *finished=job(); if(!finished || !finished->error() || finished->error()==KJob::KilledJobError || finished->error()==KIO::ERR_USER_CANCELED)return;
        const QString details=finished->errorString();
        const auto source=finished->property("finderOpenUrl").toUrl();
        if (source.isValid() && !source.isEmpty()) {
            const QString name=source.fileName().isEmpty() ? source.toDisplayString(QUrl::RemovePassword) : source.fileName();
            const QString body=finished->error()==KIO::ERR_DOES_NOT_EXIST ?
                QObject::tr("The item may have been moved or deleted, or its drive may be disconnected.") : FinderAlert::plain(details);
            FinderAlert::showError(host,QObject::tr("“%1” couldn’t be opened.").arg(name),body,details);
        } else FinderAlert::showError(host,QObject::tr("The operation couldn’t be completed."),details);
    }
private:
    QPointer<QWidget> host;
};
