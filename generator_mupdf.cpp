/***************************************************************************
 *   Copyright (C) 2008 by Pino Toscano <pino@kde.org>                     *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 ***************************************************************************/

#include "generator_mupdf.hpp"
#include "page.hpp"

#include <okular/core/page.h>
#include <okular/core/textpage.h>

#include <KLocalizedString>

#include <QFile>
#include <QImage>
#include <QMutexLocker>

OKULAR_EXPORT_PLUGIN(MuPDFGenerator, "libokularGenerator_mupdf.json")

MuPDFGenerator::MuPDFGenerator(QObject *parent, const QVariantList &args)
    : Generator(parent, args)
    , m_synopsis(0)
{
    setFeature(Threaded);
    setFeature(TextExtraction);
}

MuPDFGenerator::~MuPDFGenerator()
{
}

Okular::Document::OpenResult MuPDFGenerator::loadDocumentWithPassword(
    const QString &fileName, QVector<Okular::Page *> &pages,
    const QString &password)
{
    if (!m_pdfdoc.load(fileName)) {
        return Okular::Document::OpenError;
    }

    if (m_pdfdoc.isLocked()) {
        m_pdfdoc.unlock(password.toLocal8Bit());
        if (m_pdfdoc.isLocked()) {
            m_pdfdoc.close();
            return Okular::Document::OpenNeedsPassword;
        }
    }

    for (int i = 0; i < m_pdfdoc.pageCount(); ++i) {
        QMuPDF::Page page = m_pdfdoc.page(i);
        const QSizeF s = page.size(dpi());
        const Okular::Rotation rot = Okular::Rotation0;
        Okular::Page *okularPage = new Okular::Page(i, s.width(), s.height(), rot);
        okularPage->setDuration(page.duration());
        pages.append(okularPage);
    }

    return Okular::Document::OpenSuccess;
}

bool MuPDFGenerator::doCloseDocument()
{
    QMutexLocker locker(userMutex());
    m_pdfdoc.close();

    delete m_synopsis;
    m_synopsis = 0;

    return true;
}

Okular::DocumentInfo MuPDFGenerator::generateDocumentInfo(const QSet<Okular::DocumentInfo::Key> &keys) const
{
    QMutexLocker(userMutex());

    Okular::DocumentInfo info;
    info.set(Okular::DocumentInfo::MimeType, QStringLiteral("application/pdf"));
    info.set(Okular::DocumentInfo::Pages, QString::number(m_pdfdoc.pageCount()));
#define SET(key, val) if (keys.contains(key)) { info.set(key, val); }
    SET(Okular::DocumentInfo::Title, m_pdfdoc.infoKey("Title"));
    SET(Okular::DocumentInfo::Subject, m_pdfdoc.infoKey("Subject"));
    SET(Okular::DocumentInfo::Author, m_pdfdoc.infoKey("Author"));
    SET(Okular::DocumentInfo::Keywords, m_pdfdoc.infoKey("Keywords"));
    SET(Okular::DocumentInfo::Creator, m_pdfdoc.infoKey("Creator"));
    SET(Okular::DocumentInfo::Producer, m_pdfdoc.infoKey("Producer"));
#undef SET
    if (keys.contains(Okular::DocumentInfo::CustomKeys)) {
        info.set(QStringLiteral("format"), i18nc("PDF v. <version>", "PDF v. %1", m_pdfdoc.pdfVersion()), i18n("Format"));
    }
    return info;
}

static void recurseCreateTOC(QDomDocument &mainDoc, QMuPDF::Outline *outline,
                             QDomNode &parentDestination, const QSizeF &dpi)
{
    foreach (QMuPDF::Outline *child, outline->children()) {
        QDomElement newel = mainDoc.createElement(child->title());
        parentDestination.appendChild(newel);
        if (child->isOpen()) {
            newel.setAttribute(QStringLiteral("Open"), QStringLiteral("true"));
        }
        QMuPDF::LinkDest *link = child->link();
        if (!link) {
            continue;
        }
        switch (link->type()) {
        case QMuPDF::LinkDest::Goto: {
            QMuPDF::GotoDest *dest = static_cast<QMuPDF::GotoDest *>(link);
            Okular::DocumentViewport vp(dest->page());
            vp.rePos.pos = Okular::DocumentViewport::TopLeft;
            const QPointF p = dest->rect(dpi).topLeft();
            vp.rePos.normalizedX = p.x();
            vp.rePos.normalizedY = p.y();
            vp.rePos.enabled = true;
            newel.setAttribute(QStringLiteral("Viewport"), vp.toString());
            break;
        } case QMuPDF::LinkDest::Named: {
            QMuPDF::NamedDest *dest = static_cast<QMuPDF::NamedDest *>(link);
            newel.setAttribute(QStringLiteral("ViewportName"), dest->name());
            break;
        } case QMuPDF::LinkDest::Url: {
            QMuPDF::UrlDest *dest = static_cast<QMuPDF::UrlDest *>(link);
            newel.setAttribute(QStringLiteral("DestinationURI"), dest->address());
            break;
        }
        case QMuPDF::LinkDest::External:
        case QMuPDF::LinkDest::Launch:
            // not implemented
            break;
        default:
            break;
        }
        recurseCreateTOC(mainDoc, child, newel, dpi);
    }
}

const Okular::DocumentSynopsis *MuPDFGenerator::generateDocumentSynopsis()
{
    QMutexLocker locker(userMutex());
    if (m_synopsis) {
        return m_synopsis;
    }

    QMuPDF::Outline *outline = m_pdfdoc.outline();
    if (!outline) {
        return 0;
    }

    m_synopsis = new Okular::DocumentSynopsis();
    recurseCreateTOC(*m_synopsis, outline, *m_synopsis, dpi());
    delete outline;

    return m_synopsis;
}

QImage MuPDFGenerator::image(Okular::PixmapRequest *request)
{
    QMutexLocker locker(userMutex());

    QMuPDF::Page page = m_pdfdoc.page(request->page()->number());
    QImage image = page.render(request->width(), request->height());
    return image;
}

static Okular::TextPage *buildTextPage(const QVector<QMuPDF::TextBox *> &boxes,
                                       qreal width, qreal height)
{
    Okular::TextPage *ktp = new Okular::TextPage();
    for (int i = 0; i < boxes.size(); ++i) {
        QMuPDF::TextBox *box = boxes.at(i);
        const QChar c = box->text();
        const QRectF charBBox = box->rect();
        QString text(c);
        if (box->isAtEndOfLine()) {
            text.append(QLatin1Char('\n'));
        }
        ktp->append(text, new Okular::NormalizedRect(
                        charBBox.left() / width, charBBox.top() / height,
                        charBBox.right() / width, charBBox.bottom() / height));
    }
    return ktp;
}

Okular::TextPage *MuPDFGenerator::textPage(Okular::Page *page)
{
    QMutexLocker locker(userMutex());
    QMuPDF::Page mp = m_pdfdoc.page(page->number());
    const QVector<QMuPDF::TextBox *> boxes = mp.textBoxes(dpi());
    const QSizeF s = mp.size(dpi());

    Okular::TextPage *tp = buildTextPage(boxes, s.width(), s.height());
    qDeleteAll(boxes);
    return tp;
}

QVariant MuPDFGenerator::metaData(const QString &key,
                                  const QVariant &option) const
{
    Q_UNUSED(option)
    if (key == QStringLiteral("NamedViewport") && !option.toString().isEmpty()) {
        qWarning() << "We don't store named viewports properly, but it asked for" << option.toString();
    } else if (key == QLatin1String("DocumentTitle")) {
        QMutexLocker locker(userMutex());
        const QString title = m_pdfdoc.infoKey("Title");
        return title;
    } else if (key == QLatin1String("StartFullScreen")) {
        if (m_pdfdoc.pageMode() == QMuPDF::Document::FullScreen) {
            return true;
        }
    } else if (key == QLatin1String("OpenTOC")) {
        if (m_pdfdoc.pageMode() == QMuPDF::Document::UseOutlines) {
            return true;
        }
    }
    return QVariant();
}

#include "generator_mupdf.moc"
