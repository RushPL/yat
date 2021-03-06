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

#ifndef TEXT_SEGMENT_LINE_H
#define TEXT_SEGMENT_LINE_H

#include <QtCore/QObject>

#include "text.h"

class Screen;
class QQuickItem;

class TextStyleLine : public TextStyle {
public:
    TextStyleLine(const TextStyle &style, int start_index, int end_index)
        : start_index(start_index)
        , end_index(end_index)
        , old_index(-1)
        , text_segment(0)
        , changed(true)
    {
        this->style = style.style;
        foreground = style.foreground;
        background = style.background;
    }

    TextStyleLine()
        : start_index(0)
        , end_index(0)
        , old_index(-1)
        , text_segment(0)
        , changed(false)
    {

    }

    int start_index;
    int end_index;

    int old_index;
    Text *text_segment;
    bool changed;

    void setStyle(const TextStyle &style) {
        foreground = style.foreground;
        background = style.background;
        this->style = style.style;
    }
};

class Line : public QObject
{
    Q_OBJECT

    Q_PROPERTY(int index READ index NOTIFY indexChanged)
    Q_PROPERTY(QObject *item READ item CONSTANT)
    Q_PROPERTY(Screen *screen READ screen CONSTANT)
public:
    Line(Screen *screen);
    ~Line();

    Q_INVOKABLE Screen *screen() const;

    void releaseTextObjects();

    void clear();
    void clearToEndOfLine(int index);
    void setWidth(int width);

    Q_INVOKABLE int size() const;

    void insertAtPos(int i, const QString &text, const TextStyle &style);

    int index() const;
    void setIndex(int index);

    QString *textLine();

    QObject *item() const;

    void setVisible(bool visible);

    void dispatchEvents();
signals:
    void indexChanged();

private:

    Text *createTextSegment(const TextStyleLine &style_line);
    void releaseTextSegment(Text *text);

    Screen *m_screen;
    QString m_text_line;
    QVector<TextStyleLine> m_style_list;
    int m_index;
    int m_old_index;

    QVector<Text *> m_unused_segments;
    bool m_changed;

    QObject *m_item;
};

#endif // TEXT_SEGMENT_LINE_H
