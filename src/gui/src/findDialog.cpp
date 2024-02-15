//////////////////////////////////////////////////////////////////////////////
// BSD 3-Clause License
//
// Copyright (c) 2019, The Regents of the University of California
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the name of the copyright holder nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "findDialog.h"

#include <string>

#include "gui/gui.h"

namespace gui {
FindObjectDialog::FindObjectDialog(QWidget* parent) : QDialog(parent)
{
  setupUi(this);
}

void FindObjectDialog::accept()
{
  std::string pattern_to_find = findObjEdit->text().trimmed().toStdString();
  bool match_case = false;
  if (matchCaseCheckBox->isEnabled()) {
    match_case = matchCaseCheckBox->isChecked();
  }

  if (findObjType->currentText() == "Instance") {
    Gui::get()->select("Inst",
                       pattern_to_find,
                       "",
                       0,
                       match_case,
                       addToHighlightCheckBox->isChecked() ? 0 : -1);
  } else if (findObjType->currentText() == "Net") {
    Gui::get()->select("Net",
                       pattern_to_find,
                       "",
                       0,
                       match_case,
                       addToHighlightCheckBox->isChecked() ? 0 : -1);
  } else {
    Gui::get()->select("BTerm",
                       pattern_to_find,
                       "",
                       0,
                       match_case,
                       addToHighlightCheckBox->isChecked() ? 0 : -1);
  }

  QDialog::accept();
}

void FindObjectDialog::reject()
{
  QDialog::reject();
}

}  // namespace gui
