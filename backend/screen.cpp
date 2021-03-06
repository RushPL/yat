/**************************************************************************************************
* Copyright (c) 2012 Jørgen Lind
*
* Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
* associated documentation files (the "Software"), to deal in the Software without restriction,
* including without limitation the rights to use, copy, modify, merge, publish, distribute,
* sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all copies or
* substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
* NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
* NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
* DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT
* OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*
***************************************************************************************************/

#include "screen.h"

#include "line.h"

#include "controll_chars.h"

#include <QtCore/QTimer>
#include <QtCore/QSocketNotifier>
#include <QtGui/QGuiApplication>

#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickView>
#include <QtQml/QQmlComponent>

#include <QtCore/QDebug>

#include <float.h>

Screen::Screen(QQmlEngine *engine, QObject *parent)
    : QObject(parent)
    , m_parser(this)
    , m_timer_event_id(0)
    , m_cursor_visible(true)
    , m_cursor_visible_changed(false)
    , m_cursor_blinking(true)
    , m_cursor_blinking_changed(false)
    , m_font_metrics(m_font)
    , m_selection_valid(false)
    , m_selection_moved(0)
    , m_flash(false)
    , m_cursor_changed(false)
    , m_reset(false)
    , m_application_cursor_key_mode(false)
    , m_engine(engine)
    , m_line_component(new QQmlComponent(engine, QUrl("qrc:/qml/yat_declarative/TerminalLine.qml"),QQmlComponent::PreferSynchronous))
    , m_text_component(new QQmlComponent(engine, QUrl("qrc:/qml/yat_declarative/TextSegment.qml"),QQmlComponent::PreferSynchronous))
{
    connect(&m_pty, &YatPty::readyRead, this, &Screen::readData);

    QFont font;
    font.setFamily(QStringLiteral("Courier"));
    font.setPointSize(10);
    font.setBold(true);
    font.setHintingPreference(QFont::PreferNoHinting);
    setFont(font);

    m_screen_stack.reserve(2);

    m_cursor_stack << QPoint(0,0);

    m_current_text_style.style = TextStyle::Normal;
    m_current_text_style.foreground = ColorPalette::DefaultForground;
    m_current_text_style.background = ColorPalette::DefaultBackground;

    connect(&m_pty, SIGNAL(hangupReceived()),qGuiApp, SLOT(quit()));
}

Screen::~Screen()
{
    for (int i = 0; i < m_screen_stack.size(); i++) {
        delete m_screen_stack.at(i);

    }
    for (int i = 0; i < m_unused_text.size(); i++) {
        delete m_unused_text.at(i);
    }
}


QColor Screen::screenBackground()
{
    return QColor(Qt::black);
}

QColor Screen::defaultForgroundColor() const
{
    return m_palette.normalColor(ColorPalette::DefaultForground);
}

QColor Screen::defaultBackgroundColor() const
{
    return QColor(Qt::transparent);
}

void Screen::setHeight(int height)
{
    if (!m_screen_stack.size()) {
            m_screen_stack << new ScreenData(this);
    }

    ScreenData *data = current_screen_data();
    int size_difference = data->height() - height;

    if (!size_difference)
        return;

    data->setHeight(height);

    if (size_difference > 0) {
        if (current_cursor_y() > 0)
            current_cursor_pos().ry()--;
    }

    m_pty.setHeight(height, height * lineHeight());
    dispatchChanges();
}

void Screen::setWidth(int width)
{
    if (!m_screen_stack.size())
        m_screen_stack << new ScreenData(this);

    current_screen_data()->setWidth(width);
    m_pty.setWidth(width, width * charWidth());

}

int Screen::width() const
{
    return m_pty.size().width();
}

void Screen::saveScreenData()
{
    ScreenData *new_data = new ScreenData(this);
    QSize pty_size = m_pty.size();
    new_data->setHeight(pty_size.height());
    new_data->setWidth(pty_size.width());

    for (int i = 0; i < new_data->height(); i++) {
        current_screen_data()->at(i)->releaseTextObjects();
    }

    m_screen_stack << new_data;

    setSelectionEnabled(false);

}

void Screen::restoreScreenData()
{
    ScreenData *data = current_screen_data();
    m_screen_stack.remove(m_screen_stack.size()-1);
    delete data;

    QSize pty_size = m_pty.size();
    current_screen_data()->setHeight(pty_size.height());
    current_screen_data()->setWidth(pty_size.width());

    for (int i = 0; i < current_screen_data()->height(); i++) {
        current_screen_data()->at(i)->dispatchEvents();
    }

    setSelectionEnabled(false);
}

int Screen::height() const
{
    return current_screen_data()->height();
}

QFont Screen::font() const
{
    return m_font;
}

void Screen::setFont(const QFont &font)
{
    qreal old_width = m_font_metrics.averageCharWidth();
    qreal old_height = m_font_metrics.lineSpacing();
    m_font = font;
    m_font_metrics = QFontMetricsF(font);
    emit fontChanged();
    if (m_font_metrics.averageCharWidth() != old_width)
        emit charWidthChanged();
    if (m_font_metrics.lineSpacing() != old_height)
        emit lineHeightChanged();
}

qreal Screen::charWidth() const
{
    return m_font_metrics.averageCharWidth();
}

qreal Screen::lineHeight() const
{
    return m_font_metrics.lineSpacing();
}

void Screen::setTextStyle(TextStyle::Style style, bool add)
{
    if (add) {
        m_current_text_style.style |= style;
    } else {
        m_current_text_style.style &= !style;
    }
}

void Screen::resetStyle()
{
    m_current_text_style.background = ColorPalette::DefaultBackground;
    m_current_text_style.foreground = ColorPalette::DefaultForground;
    m_current_text_style.style = TextStyle::Normal;
}

TextStyle Screen::currentTextStyle() const
{
    return m_current_text_style;
}

TextStyle Screen::defaultTextStyle() const
{
    return { TextStyle::Normal, ColorPalette::DefaultForground, ColorPalette::DefaultBackground };
}

QPoint Screen::cursorPosition() const
{
    return QPoint(current_cursor_x(),current_cursor_y());
}

void Screen::moveCursorHome()
{
    current_cursor_pos().setX(0);
    m_cursor_changed = true;
}

void Screen::moveCursorTop()
{
    current_cursor_pos().setY(0);
    m_cursor_changed = true;
}

void Screen::moveCursorUp()
{
    current_cursor_pos().ry() -= 1;
    m_cursor_changed = true;
}

void Screen::moveCursorDown()
{
    current_cursor_pos().ry() += 1;
    m_cursor_changed = true;
}

void Screen::moveCursorLeft()
{
    current_cursor_pos().rx() -= 1;
    m_cursor_changed = true;
}

void Screen::moveCursorRight(int n_positions)
{
    current_cursor_pos().rx() += n_positions;
    m_cursor_changed = true;
}

void Screen::moveCursor(int x, int y)
{
    if (x != 0)
        x--;
    if (y != 0)
        y--;
    current_cursor_pos().setX(x);
    int height = this->height();
    if (y >= height) {
        current_cursor_pos().setY(height-1);
    } else {
        current_cursor_pos().setY(y);
    }
    m_cursor_changed = true;
}

void Screen::setCursorVisible(bool visible)
{
    m_cursor_visible = visible;
    m_cursor_visible_changed = true;
}

bool Screen::cursorVisible()
{
    return m_cursor_visible;
}

void Screen::setCursorBlinking(bool blinking)
{
    m_cursor_blinking = blinking;
    m_cursor_blinking_changed = true;
}

bool Screen::cursorBlinking()
{
    return m_cursor_blinking;
}

void Screen::saveCursor()
{
    QPoint point = current_cursor_pos();
    m_cursor_stack << point;
}

void Screen::restoreCursor()
{
    if (m_cursor_stack.size() <= 1)
        return;

    m_cursor_stack.remove(m_screen_stack.size()-1);
}

void Screen::insertAtCursor(const QString &text)
{
    if (m_selection_valid ) {
        if (current_cursor_y() >= m_selection_start.y() && current_cursor_y() <= m_selection_end.y())
            //don't need to schedule as event since it will only happen once
            setSelectionEnabled(false);
    }

    if (current_cursor_x() + text.size() <= width()) {
        Line *line = current_screen_data()->at(current_cursor_y());
        line->insertAtPos(current_cursor_x(), text, m_current_text_style);
        current_cursor_pos().rx() += text.size();
    } else {
        for (int i = 0; i < text.size();) {
            if (current_cursor_x() == width()) {
                current_cursor_pos().setX(0);
                lineFeed();
            }
            QString toLine = text.mid(i,current_screen_data()->width() - current_cursor_x());
            Line *line = current_screen_data()->at(current_cursor_y());
            line->insertAtPos(current_cursor_x(),toLine, m_current_text_style);
            i+= toLine.size();
            current_cursor_pos().rx() += toLine.size();
        }
    }

    m_cursor_changed = true;
}

void Screen::backspace()
{
    current_cursor_pos().rx()--;
    m_cursor_changed = true;
}

void Screen::eraseLine()
{
    current_screen_data()->clearLine(current_cursor_y());
}

void Screen::eraseFromCursorPositionToEndOfLine()
{
    current_screen_data()->clearToEndOfLine(current_cursor_y(), current_cursor_x());
}

void Screen::eraseFromCurrentLineToEndOfScreen()
{
    current_screen_data()->clearToEndOfScreen(current_cursor_y());

}

void Screen::eraseFromCurrentLineToBeginningOfScreen()
{
    current_screen_data()->clearToBeginningOfScreen(current_cursor_y());

}

void Screen::eraseToCursorPosition()
{
    qDebug() << "eraseToCursorPosition NOT IMPLEMENTED!";
}

void Screen::eraseScreen()
{
    current_screen_data()->clear();
}

void Screen::setTextStyleColor(ushort color)
{
    Q_ASSERT(color >= 30 && color < 50);
    if (color < 38) {
        m_current_text_style.foreground = ColorPalette::Color(color - 30);
    } else if (color == 39) {
        m_current_text_style.foreground = ColorPalette::DefaultForground;
    } else if (color >= 40 && color < 48) {
        m_current_text_style.background = ColorPalette::Color(color - 40);
    } else if (color == 49) {
        m_current_text_style.background = ColorPalette::DefaultBackground;
    } else {
        qDebug() << "Failed to set color";
    }
}

const ColorPalette *Screen::colorPalette() const
{
    return &m_palette;
}

void Screen::lineFeed()
{
    int cursor_y = current_cursor_y();
    if(cursor_y == current_screen_data()->scrollAreaEnd()) {
        m_selection_start.ry()--;
        m_selection_end.ry()--;
        m_selection_moved = true;
        moveLine(current_screen_data()->scrollAreaStart(),cursor_y);
    } else {
        current_cursor_pos().ry()++;
        m_cursor_changed = true;
    }
}

void Screen::reverseLineFeed()
{
    int cursor_y = current_cursor_y();
    if (cursor_y == current_screen_data()->scrollAreaStart()) {
        m_selection_start.ry()++;
        m_selection_end.ry()++;
        m_selection_moved = true;
        moveLine(current_screen_data()->scrollAreaEnd(), cursor_y);
    } else {
        current_cursor_pos().ry()--;
        m_cursor_changed = true;
    }
}

void Screen::insertLines(int count)
{
    for (int i = 0; i < count; i++) {
        moveLine(current_screen_data()->scrollAreaEnd(),current_cursor_y());
    }
}

void Screen::deleteLines(int count)
{
    for (int i = 0; i < count; i++) {
        moveLine(current_cursor_y(),current_screen_data()->scrollAreaEnd());
    }

}

void Screen::setScrollArea(int from, int to)
{
    from--;
    to--;
    current_screen_data()->setScrollArea(from,to);
}

QPointF Screen::selectionAreaStart() const
{
    return m_selection_start;
}

void Screen::setSelectionAreaStart(const QPointF &start)
{
    bool emitChanged = m_selection_start != start;
    m_selection_start = start;
    setSelectionValidity();
    if (emitChanged)
        emit selectionAreaStartChanged();
}

QPointF Screen::selectionAreaEnd() const
{
    return m_selection_end;
}

void Screen::setSelectionAreaEnd(const QPointF &end)
{
    bool emitChanged = m_selection_end != end;
    m_selection_end = end;
    setSelectionValidity();
    if (emitChanged)
        emit selectionAreaEndChanged();
}

bool Screen::selectionEnabled() const
{
    return m_selection_valid;
}

void Screen::setSelectionEnabled(bool enabled)
{
    bool emitchanged = m_selection_valid != enabled;
    m_selection_valid = enabled;
    if (emitchanged)
        emit selectionEnabledChanged();
}

void Screen::sendSelectionToClipboard() const
{
    current_screen_data()->sendSelectionToClipboard(m_selection_start, m_selection_end, QClipboard::Clipboard);
}

void Screen::sendSelectionToSelection() const
{
    current_screen_data()->sendSelectionToClipboard(m_selection_start, m_selection_end, QClipboard::Selection);
}

void Screen::pasteFromSelection()
{
    m_pty.write(QGuiApplication::clipboard()->text(QClipboard::Selection).toUtf8());
}

void Screen::pasteFromClipboard()
{
    m_pty.write(QGuiApplication::clipboard()->text(QClipboard::Clipboard).toUtf8());
}

void Screen::doubleClicked(const QPointF &clicked)
{
    int start, end;
    current_screen_data()->getDoubleClickSelectionArea(clicked, &start, &end);
    setSelectionAreaStart(QPointF(start,clicked.y()));
    setSelectionAreaEnd(QPointF(end,clicked.y()));
}

void Screen::setTitle(const QString &title)
{
    m_title = title;
    emit screenTitleChanged();
}

QString Screen::title() const
{
    return m_title;
}

void Screen::scheduleFlash()
{
    m_flash = true;
}

Line *Screen::at(int i) const
{
    return current_screen_data()->at(i);
}

void Screen::printScreen() const
{
    current_screen_data()->printScreen();
}

void Screen::dispatchChanges()
{
    if (m_reset) {
        emit reset();
        m_update_actions.clear();
        m_reset = false;
    } else {
        qint16 begin_move = -1;
        qint16 end_move = -1;
        for (int i = 0; i < m_update_actions.size(); i++) {
            UpdateAction action = m_update_actions.at(i);
            switch(action.action) {
            case UpdateAction::MoveLine: {
                if (begin_move < 0) {
                    begin_move = qMin(action.to_line, action.from_line);
                    end_move = qMax(action.to_line, action.from_line);
                } else
                    if (action.from_line > action.to_line) {
                        begin_move = qMin(action.to_line, begin_move);
                        end_move = qMax(action.from_line, end_move);
                    } else {
                        begin_move = qMin(action.from_line, begin_move);
                        end_move = qMax(action.to_line, end_move);
                    }
            }
                break;
            default:
                qDebug() << "unhandeled UpdatAction in TerminalScreen";
                break;
            }
        }

        if (begin_move >= 0) {
            current_screen_data()->updateIndexes(begin_move, end_move);
        }

        current_screen_data()->dispatchLineEvents();
        emit dispatchTextSegmentChanges();
        for (int i = 0; i < m_unused_text.size(); i++) {
            m_unused_text.at(i)->dispatchEvents();
        }
    }

    if (m_flash) {
        m_flash = false;
        emit flash();
    }

    if (m_cursor_changed) {
        m_cursor_changed = false;
        emit cursorPositionChanged(current_cursor_x(), current_cursor_y());
    }

    if (m_cursor_visible_changed) {
        m_cursor_visible_changed = false;
        emit cursorVisibleChanged();
    }

    if (m_cursor_blinking_changed) {
        m_cursor_blinking_changed = false;
        emit cursorBlinkingChanged();

    }

    if (m_selection_valid && m_selection_moved) {
        if (m_selection_start.y() < 0 ||
                m_selection_end.y() >= current_screen_data()->height()) {
            setSelectionEnabled(false);
        } else {
            emit selectionAreaStartChanged();
            emit selectionAreaEndChanged();
        }
    }

    m_update_actions.clear();

    m_time_since_dispatched.restart();
}

void Screen::sendPrimaryDA()
{
    m_pty.write(QByteArrayLiteral("\033[?6c"));

}

void Screen::sendSecondaryDA()
{
    m_pty.write(QByteArrayLiteral("\033[>1;95;0c"));
}

void Screen::setCharacterMap(const QString &string)
{
    m_character_map = string;
}

QString Screen::characterMap() const
{
    return m_character_map;
}

void Screen::setApplicationCursorKeysMode(bool enable)
{
    m_application_cursor_key_mode = enable;
}

bool Screen::applicationCursorKeyMode() const
{
    return m_application_cursor_key_mode;
}

void Screen::sendKey(const QString &text, Qt::Key key, Qt::KeyboardModifiers modifiers)
{
    /// UGH, this functions should be re-written
    char escape = '\0';
    char  control = '\0';
    char  code = '\0';
    QVector<ushort> parameters;
    bool found = true;

    switch(key) {
    case Qt::Key_Up:
        escape = C0::ESC;
        if (m_application_cursor_key_mode)
            control = C1_7bit::SS3;
        else
            control = C1_7bit::CSI;

        code = 'A';
        break;
    case Qt::Key_Right:
        escape = C0::ESC;
        if (m_application_cursor_key_mode)
            control = C1_7bit::SS3;
        else
            control = C1_7bit::CSI;

        code = 'C';
        break;
    case Qt::Key_Down:
        escape = C0::ESC;
        if (m_application_cursor_key_mode)
            control = C1_7bit::SS3;
        else
            control = C1_7bit::CSI;

            code = 'B';
        break;
    case Qt::Key_Left:
        escape = C0::ESC;
        if (m_application_cursor_key_mode)
            control = C1_7bit::SS3;
        else
            control = C1_7bit::CSI;

        code = 'D';
        break;
    case Qt::Key_Insert:
        escape = C0::ESC;
        control = C1_7bit::CSI;
        parameters.append(2);
        code = '~';
        break;
    case Qt::Key_Delete:
        escape = C0::ESC;
        control = C1_7bit::CSI;
        parameters.append(3);
        code = '~';
        break;
    case Qt::Key_Home:
        escape = C0::ESC;
        control = C1_7bit::CSI;
        parameters.append(1);
        code = '~';
        break;
    case Qt::Key_End:
        escape = C0::ESC;
        control = C1_7bit::CSI;
        parameters.append(4);
        code = '~';
        break;
    case Qt::Key_PageUp:
        escape = C0::ESC;
        control = C1_7bit::CSI;
        parameters.append(5);
        code = '~';
        break;
    case Qt::Key_PageDown:
        escape = C0::ESC;
        control = C1_7bit::CSI;
        parameters.append(6);
        code = '~';
        break;
    case Qt::Key_F1:
    case Qt::Key_F2:
    case Qt::Key_F3:
    case Qt::Key_F4:
        if (m_application_cursor_key_mode) {
            parameters.append((key & 0xff) - 37);
            escape = C0::ESC;
            control = C1_7bit::CSI;
            code = '~';
        }
        break;
    case Qt::Key_F5:
    case Qt::Key_F6:
    case Qt::Key_F7:
    case Qt::Key_F8:
    case Qt::Key_F9:
    case Qt::Key_F10:
    case Qt::Key_F11:
    case Qt::Key_F12:
        if (m_application_cursor_key_mode) {
            parameters.append((key & 0xff) - 36);
            escape = C0::ESC;
            control = C1_7bit::CSI;
            code = '~';
        }
        break;
    case Qt::Key_Control:
    case Qt::Key_Shift:
    case Qt::Key_Alt:
    case Qt::Key_AltGr:
        return;
        break;
    default:
        found = false;
    }

    if (found) {
        int term_mods = 0;
        if (modifiers & Qt::ShiftModifier)
            term_mods |= 1;
        if (modifiers & Qt::AltModifier)
            term_mods |= 2;
        if (modifiers & Qt::ControlModifier)
            term_mods |= 4;

        QByteArray toPty;

        if (term_mods) {
            term_mods++;
            parameters.append(term_mods);
        }
        if (escape)
            toPty.append(escape);
        if (control)
            toPty.append(control);
        if (parameters.size()) {
            for (int i = 0; i < parameters.size(); i++) {
                if (i)
                    toPty.append(';');
                toPty.append(QByteArray::number(parameters.at(i)));
            }
        }
        if (code)
            toPty.append(code);
        m_pty.write(toPty);

    } else {
        QByteArray to_pty;
        QByteArray key_text;
        if (modifiers & Qt::ControlModifier) {
            char key_char = text.toLocal8Bit().at(0);
            key_text.append(key_char & 0x1F);

        } else {
            key_text = text.toUtf8();
        }

        if (modifiers &  Qt::AltModifier) {
            to_pty.append(C0::ESC);
        } else if (modifiers & Qt::MetaModifier) {
            to_pty.append(C0::ESC);
            to_pty.append('@');
            to_pty.append(FinalBytesNoIntermediate::Reserved3);
        }

        to_pty.append(key_text);
        m_pty.write(to_pty);
    }
}

QObject *Screen::createLineItem()
{
    return m_line_component->create();
}

void Screen::destroyLineItem(QObject *lineItem)
{
    lineItem->deleteLater();
}

QObject *Screen::createTextItem()
{
    return m_text_component->create();
}

void Screen::destroyTextItem(QObject *textItem)
{
    textItem->deleteLater();
}

Text *Screen::createText()
{
    if (m_unused_text.size())
        return m_unused_text.takeLast();

    return new Text(this);
}

void Screen::releaseText(Text *text)
{
    m_unused_text << text;
}

YatPty *Screen::pty()
{
    return &m_pty;
}

void Screen::readData(const QByteArray &data)
{
    m_parser.addData(data);

    if (!m_timer_event_id)
        m_timer_event_id = startTimer(14);
    m_time_since_parsed.restart();
}

void Screen::moveLine(qint16 from, qint16 to)
{
    current_screen_data()->moveLine(from,to);
    scheduleMoveSignal(from,to);
}

void Screen::scheduleMoveSignal(qint16 from, qint16 to)
{
    if (m_update_actions.size() &&
            m_update_actions.last().action == UpdateAction::MoveLine &&
            m_update_actions.last().from_line == from &&
            m_update_actions.last().to_line == to) {
        m_update_actions.last().count++;
    } else {
        m_update_actions << UpdateAction(UpdateAction::MoveLine, from, to, 1);
    }
}


void Screen::setSelectionValidity()
{
    if (m_selection_end.y() > m_selection_start.y() ||
            (m_selection_end.y() == m_selection_start.y() &&
             m_selection_end.x() > m_selection_start.x())) {
        setSelectionEnabled(true);
    } else {
        setSelectionEnabled(false);
    }
}


void Screen::timerEvent(QTimerEvent *)
{
    if (m_timer_event_id && m_time_since_parsed.elapsed() > 15 ||
            (!m_time_since_dispatched.isValid() || m_time_since_dispatched.elapsed() > 50)) {
        killTimer(m_timer_event_id);
        m_timer_event_id = 0;
        dispatchChanges();
    }
}
