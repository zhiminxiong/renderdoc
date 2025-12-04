/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2019-2025 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "CameraControlsDialog.h"
#include <QKeyEvent>
#include <QMetaEnum>
#include <QMouseEvent>
#include "Code/QRDUtils.h"
#include "ui_CameraControlsDialog.h"

CameraControlsDialog::CameraControlsDialog(ICaptureContext &Ctx, QWidget *parent)
    : QDialog(parent), m_Ctx(Ctx), ui(new Ui::CameraControlsDialog)
{
  ui->setupUi(this);
  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

  ui->nearPlane->setValue(m_Ctx.Config().MeshViewer_CameraNear);
  ui->farPlane->setValue(m_Ctx.Config().MeshViewer_CameraFar);

  if(m_Ctx.Config().MeshViewer_KeySettings.size() < (size_t)KeyPressDirection::NumSettings)
  {
    m_Keys.clear();
  }
  else
  {
    m_Keys = m_Ctx.Config().MeshViewer_KeySettings;
  }

  updateDisplayLabels();

  ui->speedMod->setCurrentIndex(0);
  if(m_Ctx.Config().MeshViewer_SpeedModifier == Qt::ShiftModifier)
    ui->speedMod->setCurrentIndex(0);
  else if(m_Ctx.Config().MeshViewer_SpeedModifier == Qt::AltModifier)
    ui->speedMod->setCurrentIndex(1);
  else if(m_Ctx.Config().MeshViewer_SpeedModifier == Qt::ControlModifier)
    ui->speedMod->setCurrentIndex(2);
  else if(m_Ctx.Config().MeshViewer_SpeedModifier == Qt::NoModifier)
    ui->speedMod->setCurrentIndex(3);

  m_Keys.resize((size_t)KeyPressDirection::NumSettings);

  for(QToolButton *b : findChildren<QToolButton *>())
    connect(b, &QToolButton::clicked, this, &CameraControlsDialog::setKey);

  connect(ui->buttonBox, &QDialogButtonBox::accepted, this,
          &CameraControlsDialog::applyUpdatedControls);
  connect(ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

  {
    m_KeybindDialog = new QDialog(this);
    m_KeybindDialog->setWindowTitle(tr("Make Key bind"));
    m_KeybindDialog->setWindowFlags(m_KeybindDialog->windowFlags() & ~Qt::WindowContextHelpButtonHint);
    m_KeybindDialog->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_KeybindDialog->setFixedSize(200, 80);

    QDialogButtonBox *buttons = new QDialogButtonBox(m_KeybindDialog);
    buttons->addButton(QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::rejected, m_KeybindDialog, &QDialog::reject);

    QLabel *instructions = new QLabel(m_KeybindDialog);
    instructions->setText(tr("Press key or mouse button, or escape to clear binding."));
    instructions->setWordWrap(true);

    QVBoxLayout *layout = new QVBoxLayout(m_KeybindDialog);

    layout->addWidget(instructions);
    layout->addWidget(buttons);

    m_KeybindDialog->setLayout(layout);

    m_KeybindDialog->installEventFilter(this);
  }
}

void CameraControlsDialog::applyUpdatedControls()
{
  bool someSet = false;

  m_Ctx.Config().MeshViewer_CameraNear = (float)ui->nearPlane->value();
  m_Ctx.Config().MeshViewer_CameraFar = (float)ui->farPlane->value();

  for(size_t i = 0; i < m_Keys.size() && i < (size_t)KeyPressDirection::NumSettings; i++)
  {
    if(m_Keys[i])
    {
      someSet = true;
      break;
    }
  }

  switch(ui->speedMod->currentIndex())
  {
    case 0: m_Ctx.Config().MeshViewer_SpeedModifier = Qt::ShiftModifier; break;
    case 1: m_Ctx.Config().MeshViewer_SpeedModifier = Qt::AltModifier; break;
    case 2: m_Ctx.Config().MeshViewer_SpeedModifier = Qt::ControlModifier; break;
    default: m_Ctx.Config().MeshViewer_SpeedModifier = Qt::NoModifier; break;
  }

  if(!someSet)
    m_Keys.clear();

  m_Ctx.Config().MeshViewer_KeySettings = m_Keys;

  m_Ctx.Config().Save();
  accept();
}

void CameraControlsDialog::on_resetAll_clicked()
{
  m_Keys.clear();

  updateDisplayLabels();
}

void CameraControlsDialog::setKey()
{
  m_Keybind = 0;
  RDDialog::show(m_KeybindDialog);

  // find the corresponding display label for this button
  QLineEdit *display =
      findChild<QLineEdit *>(QObject::sender()->objectName().replace(lit("Set"), lit("Display")));

  const QLineEdit *labels[(size_t)KeyPressDirection::NumSettings] = {
      // KeyPressDirection::Forward,
      ui->forwardDisplay,
      ui->forwardDisplay_2,
      // KeyPressDirection::Back,
      ui->backwardDisplay,
      ui->backwardDisplay_2,
      // KeyPressDirection::Left,
      ui->leftDisplay,
      ui->leftDisplay_2,
      // KeyPressDirection::Right,
      ui->rightDisplay,
      ui->rightDisplay_2,
      // KeyPressDirection::Up,
      ui->upDisplay,
      ui->upDisplay_2,
      // KeyPressDirection::Down,
      ui->downDisplay,
      ui->downDisplay_2,
  };

  int keyIdx = -1;
  for(int i = 0; i < (int)ARRAY_COUNT(labels); i++)
  {
    if(labels[i] == display)
    {
      keyIdx = i;
      break;
    }
  }

  if(keyIdx < 0)
  {
    qCritical() << "Couldn't identify key being bound";
    return;
  }

  if(m_Keybind == 0)
  {
    // cancelled, do nothing
    return;
  }
  else if(getKeySetting(m_Keybind) == Qt::Key_Escape)
  {
    m_Keys[keyIdx] = 0;
  }
  else
  {
    m_Keys[keyIdx] = m_Keybind;
  }

  updateDisplayLabels();
}

bool CameraControlsDialog::eventFilter(QObject *watched, QEvent *event)
{
  if(event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride)
  {
    QKeyEvent *key = (QKeyEvent *)event;

    if(key->key() != 0)
    {
      m_Keybind = makeKeySetting((Qt::Key)key->key());
      m_KeybindDialog->accept();
      event->accept();
      return true;
    }
  }
  else if(event->type() == QEvent::MouseButtonPress)
  {
    QMouseEvent *mouse = (QMouseEvent *)event;
    if(mouse->button() != Qt::LeftButton && mouse->button() != Qt::RightButton &&
       mouse->button() != Qt::MiddleButton)
    {
      m_Keybind = makeMouseButtonSetting(mouse->button());
      m_KeybindDialog->accept();
      event->accept();
      return true;
    }
  }
  else if(event->type() == QEvent::Wheel)
  {
    QWheelEvent *mouse = (QWheelEvent *)event;

    m_Keybind = makeMouseWheelSetting(mouse->angleDelta());
    m_KeybindDialog->accept();
    event->accept();
    return true;
  }

  return QObject::eventFilter(watched, event);
}

QString CameraControlsDialog::buttonName(Qt::MouseButton button)
{
  switch(button)
  {
    case Qt::NoButton: return tr("No mouse button");
    case Qt::LeftButton: return tr("Left mouse");
    case Qt::MiddleButton: return tr("Middle mouse");
    case Qt::RightButton: return tr("Right mouse");
    case Qt::BackButton: return tr("Mouse back");
    case Qt::ForwardButton: return tr("Mouse forward");
    case Qt::TaskButton: return tr("Mouse task");
    case Qt::ExtraButton4: return tr("Mouse 7");
    case Qt::ExtraButton5: return tr("Mouse 8");
    case Qt::ExtraButton6: return tr("Mouse 9");
    case Qt::ExtraButton7: return tr("Mouse 10");
    case Qt::ExtraButton8: return tr("Mouse 11");
    case Qt::ExtraButton9: return tr("Mouse 12");
    case Qt::ExtraButton10: return tr("Mouse 13");
    case Qt::ExtraButton11: return tr("Mouse 14");
    case Qt::ExtraButton12: return tr("Mouse 15");
    case Qt::ExtraButton13: return tr("Mouse 16");
    case Qt::ExtraButton14: return tr("Mouse 17");
    case Qt::ExtraButton15: return tr("Mouse 18");
    case Qt::ExtraButton16: return tr("Mouse 19");
    case Qt::ExtraButton17: return tr("Mouse 20");
    case Qt::ExtraButton18: return tr("Mouse 21");
    case Qt::ExtraButton19: return tr("Mouse 22");
    case Qt::ExtraButton20: return tr("Mouse 23");
    case Qt::ExtraButton21: return tr("Mouse 24");
    case Qt::ExtraButton22: return tr("Mouse 25");
    case Qt::ExtraButton23: return tr("Mouse 26");
    case Qt::ExtraButton24: return tr("Mouse 27");
    default: return tr("Unknown button");
  }
}

QString CameraControlsDialog::wheelName(QPoint angleDelta)
{
  if(angleDelta.y() > 0)
    return tr("Mousewheel up");
  else if(angleDelta.y() < 0)
    return tr("Mousewheel down");
  else if(angleDelta.x() < 0)
    return tr("Mousewheel left");
  else if(angleDelta.x() > 0)
    return tr("Mousewheel right");
  return tr("Unknown wheel");
}

void CameraControlsDialog::updateDisplayLabels()
{
  bool someSet = false;

  for(size_t i = 0; i < m_Keys.size() && i < (size_t)KeyPressDirection::NumSettings; i++)
  {
    if(m_Keys[i])
    {
      someSet = true;
      break;
    }
  }

  QLineEdit *labels[(size_t)KeyPressDirection::NumSettings] = {
      // KeyPressDirection::Forward,
      ui->forwardDisplay,
      ui->forwardDisplay_2,
      // KeyPressDirection::Back,
      ui->backwardDisplay,
      ui->backwardDisplay_2,
      // KeyPressDirection::Left,
      ui->leftDisplay,
      ui->leftDisplay_2,
      // KeyPressDirection::Right,
      ui->rightDisplay,
      ui->rightDisplay_2,
      // KeyPressDirection::Up,
      ui->upDisplay,
      ui->upDisplay_2,
      // KeyPressDirection::Down,
      ui->downDisplay,
      ui->downDisplay_2,
  };

  if(someSet)
  {
    for(size_t i = 0; i < (size_t)KeyPressDirection::NumSettings; i++)
    {
      if(m_Keys[i] == 0)
      {
        labels[i]->setText(tr("Unbound"));
      }
      else
      {
        QString displayString;
        if(getKeySetting(m_Keys[i]) != Qt::Key_unknown)
        {
          displayString = QKeySequence(getKeySetting(m_Keys[i])).toString();
        }
        else if(getMouseButtonSetting(m_Keys[i]) != Qt::MaxMouseButton)
        {
          displayString = buttonName(getMouseButtonSetting(m_Keys[i]));
        }
        else if(getMouseWheelSetting(m_Keys[i]) != QPoint())
        {
          displayString = wheelName(getMouseWheelSetting(m_Keys[i]));
        }

        labels[i]->setText(displayString);
      }
    }
  }
  else
  {
    for(size_t i = 0; i < (size_t)KeyPressDirection::Count; i++)
    {
      labels[i * 2 + 0]->setText(tr("Default"));
      labels[i * 2 + 1]->setText(QString());
    }
  }
}

CameraControlsDialog::~CameraControlsDialog()
{
  delete ui;
}
