#include "editor_ai_helpers.h"

#include <QCryptographicHash>
#include <QPainter>
#include <QRegularExpression>

namespace {

QString initialsFromIdentity(const QString& identity)
{
    const QString token = identity.section(QLatin1Char('@'), 0, 0).trimmed();
    if (token.isEmpty()) {
        return QStringLiteral("U");
    }
    const QStringList pieces = token.split(QRegularExpression(QStringLiteral("[._\\-\\s]+")),
                                           Qt::SkipEmptyParts);
    QString initials;
    for (const QString& piece : pieces) {
        if (!piece.isEmpty()) {
            initials += piece.left(1).toUpper();
            if (initials.size() >= 2) {
                break;
            }
        }
    }
    if (initials.isEmpty()) {
        initials = token.left(1).toUpper();
    }
    return initials.left(2);
}

}  // namespace

QPixmap buildFallbackAvatar(const QString& identity)
{
    constexpr int size = 28;
    QPixmap pix(size, size);
    pix.fill(Qt::transparent);

    const QByteArray hash = QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Md5);
    QColor color(0x3d, 0x7c, 0xc9);
    if (hash.size() >= 3) {
        color = QColor(static_cast<uchar>(hash[0]), static_cast<uchar>(hash[1]), static_cast<uchar>(hash[2]));
        color = color.lighter(130);
    }

    QPainter painter(&pix);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(color);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(0, 0, size, size);
    painter.setPen(Qt::white);
    QFont font = painter.font();
    font.setBold(true);
    font.setPointSize(10);
    painter.setFont(font);
    painter.drawText(QRect(0, 0, size, size), Qt::AlignCenter, initialsFromIdentity(identity));
    return pix;
}

QString normalizeBaseUrl(QString value)
{
    value = value.trimmed();
    while (value.endsWith(QLatin1Char('/'))) {
        value.chop(1);
    }
    return value;
}
