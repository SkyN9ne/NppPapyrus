/*
This file is part of Papyrus Plugin for Notepad++.

Copyright (C) 2021 blu3mania <blu3mania@hotmail.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "ErrorAnnotator.hpp"

#include "..\Common\Utility.hpp"

#include "..\..\external\gsl\include\gsl\util"
#include "..\..\external\npp\Common.h"

#include <list>
#include <map>
#include <string>

namespace papyrus {

  ErrorAnnotator::ErrorAnnotator(const NppData& nppData, ErrorAnnotatorSettings& settings)
    : nppData(nppData), settings(settings) {
    // Setup settings change listeners
    settings.enableAnnotation.subscribe([&](auto eventData) { updateAnnotationStyle(); });
    settings.annotationForegroundColor.subscribe([&](auto eventData) { updateAnnotationStyle(); });
    settings.annotationBackgroundColor.subscribe([&](auto eventData) { updateAnnotationStyle(); });
    settings.isAnnotationItalic.subscribe([&](auto eventData) { updateAnnotationStyle(); });
    settings.isAnnotationBold.subscribe([&](auto eventData) { updateAnnotationStyle(); });

    settings.enableIndication.subscribe([&](auto eventData) { updateIndicatorStyle(); });
    settings.indicatorID.subscribe([&](auto eventData) { changeIndicator(eventData.newValue); });
    settings.indicatorStyle.subscribe([&](auto eventData) { updateIndicatorStyle(); });
    settings.indicatorForegroundColor.subscribe([&](auto eventData) { updateIndicatorStyle(); });
  }

  ErrorAnnotator::~ErrorAnnotator() {
    if (mainViewStyleAssigned != 0) {
      ::SendMessage(nppData._scintillaMainHandle, SCI_RELEASEALLEXTENDEDSTYLES, 0, 0);
    }

    if (secondViewStyleAssigned != 0) {
      ::SendMessage(nppData._scintillaSecondHandle, SCI_RELEASEALLEXTENDEDSTYLES, 0, 0);
    }
  }

  void ErrorAnnotator::clear() {
    errors.clear();

    // Check and clear all annotations from both views
    if (!getApplicableFilePathOnView(MAIN_VIEW).empty()) {
      clearAnnotations(nppData._scintillaMainHandle);
      clearIndications(nppData._scintillaMainHandle);
    }
    if (!getApplicableFilePathOnView(SUB_VIEW).empty()) {
      clearAnnotations(nppData._scintillaSecondHandle);
      clearIndications(nppData._scintillaSecondHandle);
    }
  }

  void ErrorAnnotator::annotate(const std::vector<Error>& compilationErrors) {
    for (const auto& error : compilationErrors) {
      std::wstring key = utility::toUpper(error.file);
      if (!hasErrors(key)) {
        errors[key] = FileErrors();
      }

      auto& errorList = errors[key];
      auto iter = std::find_if(errorList.begin(), errorList.end(),
        [&](LineError& lineError) {
          // Scintilla's line # is zero-based
          return lineError.line == error.line - 1;
        }
      );
      if (iter == errorList.end()) {
        LineError lineError {
          .line = error.line - 1, // Scintilla's line # is zero-based
          .message = wstring2string(L"Error: " + error.message, SC_CP_UTF8), // Scintilla does not use wide char
          .columns { error.column }
        };
        errorList.push_back(lineError);
      } else {
        iter->message += "\r\n" + wstring2string(L"Error: " + error.message, SC_CP_UTF8);
        iter->columns.push_back(error.column);
      }
    }

    annotate(MAIN_VIEW);
    annotate(SUB_VIEW);
  }

  void ErrorAnnotator::annotate(npp_view_t view, std::wstring filePath) {
    indicatorID = settings.indicatorID;
    HWND handle = (view == MAIN_VIEW ? nppData._scintillaMainHandle : nppData._scintillaSecondHandle);

    // Check if current file has errors
    auto fileErrors = errors.find(utility::toUpper(filePath));
    if (fileErrors != errors.end()) {
      // Update annotation style
      updateAnnotationStyle(view, handle);

      // Update indicator style
      updateIndicatorStyle(handle);

      for (const LineError& lineError : fileErrors->second) {
        // Annotation
        drawAnnotations(handle, lineError);

        // Indicator
        drawIndications(handle, lineError);
      }
    } else {
      clearAnnotations(handle);
      clearIndications(handle);
    }
  }

  // Private methods
  //

  bool ErrorAnnotator::hasErrors(const std::wstring& filePath) const {
    return errors.find(utility::toUpper(filePath)) != errors.end();
  }

  std::wstring ErrorAnnotator::getApplicableFilePathOnView(npp_view_t view) const {
    // Check whether there is an active doc on the given view
    HWND handle = (view == MAIN_VIEW ? nppData._scintillaMainHandle : nppData._scintillaSecondHandle);
    npp_index_t docIndex = static_cast<npp_index_t>(::SendMessage(nppData._nppHandle, NPPM_GETCURRENTDOCINDEX, 0, static_cast<LPARAM>(view)));
    if (docIndex != -1) {
      npp_buffer_t bufferID = static_cast<npp_buffer_t>(::SendMessage(nppData._nppHandle, NPPM_GETBUFFERIDFROMPOS, static_cast<WPARAM>(docIndex), static_cast<LPARAM>(view)));
      if (bufferID != 0) {
        // Make sure it is a Papyrus script
        wchar_t filePathArray[MAX_PATH];
        if (::SendMessage(nppData._nppHandle, NPPM_GETFULLPATHFROMBUFFERID, static_cast<WPARAM>(bufferID), reinterpret_cast<LPARAM>(filePathArray)) != -1) {
          std::wstring filePath(filePathArray);
          if (utility::endsWith(filePath, L".psc") || utility::endsWith(filePath, L".pas")) {
            return filePath;
          }
        }
      }
    }

    return std::wstring();
  }

  void ErrorAnnotator::annotate(npp_view_t view) {
    // Annotate current file on the given view if it's Papyrus script
    std::wstring filePath = getApplicableFilePathOnView(view);
    if (!filePath.empty()) {
      annotate(view, filePath);
    } else {
      HWND handle = (view == MAIN_VIEW ? nppData._scintillaMainHandle : nppData._scintillaSecondHandle);
      clearAnnotations(handle);
      clearIndications(handle);
    }
  }

  void ErrorAnnotator::clearAnnotations(HWND handle) const {
    ::SendMessage(handle, SCI_ANNOTATIONCLEARALL, 0, 0);
  }

  void ErrorAnnotator::clearIndications(HWND handle) const {
    if (indicatorID > 0) {
      // Need to specify which indicator to be cleared
      ::SendMessage(handle, SCI_SETINDICATORCURRENT, indicatorID, 0);
      npp_length_t docSize = ::SendMessage(handle, SCI_GETLENGTH, 0, 0);
      ::SendMessage(handle, SCI_INDICATORCLEARRANGE, 0, docSize);
    }
  }

  void ErrorAnnotator::showAnnotations(HWND handle) const {
    ::SendMessage(handle, SCI_ANNOTATIONSETVISIBLE, ANNOTATION_BOXED, 0);
  }

  void ErrorAnnotator::hideAnnotations(HWND handle) const {
    ::SendMessage(handle, SCI_ANNOTATIONSETVISIBLE, ANNOTATION_HIDDEN, 0);
  }

  void ErrorAnnotator::showIndications(HWND handle) const {
    ::SendMessage(handle, SCI_INDICSETSTYLE, indicatorID, settings.indicatorStyle);
  }

  void ErrorAnnotator::hideIndications(HWND handle) const {
    ::SendMessage(handle, SCI_INDICSETSTYLE, indicatorID, INDIC_HIDDEN);
  }

  void ErrorAnnotator::updateAnnotationStyle() {
    // Update annotation style of the current file on the given view if it's Papyrus script
    if (!getApplicableFilePathOnView(MAIN_VIEW).empty()) {
      updateAnnotationStyle(MAIN_VIEW, nppData._scintillaMainHandle);
    }
    if (!getApplicableFilePathOnView(SUB_VIEW).empty()) {
      updateAnnotationStyle(SUB_VIEW, nppData._scintillaSecondHandle);
    }
  }

  void ErrorAnnotator::updateAnnotationStyle(npp_view_t view, HWND handle) {
    // Get a style assigned if needed.
    int& styleAssigned = (view == MAIN_VIEW ? mainViewStyleAssigned : secondViewStyleAssigned);
    if (styleAssigned == 0) {
      // Request to allocate one style from Scintilla.
      styleAssigned = static_cast<int>(::SendMessage(handle, SCI_ALLOCATEEXTENDEDSTYLES, 1, 0));
      ::SendMessage(handle, SCI_ANNOTATIONSETSTYLEOFFSET, styleAssigned, 0);
    }

    ::SendMessage(handle, SCI_STYLESETFORE, styleAssigned, settings.annotationForegroundColor);
    ::SendMessage(handle, SCI_STYLESETBACK, styleAssigned, settings.annotationBackgroundColor);
    ::SendMessage(handle, SCI_STYLESETITALIC, styleAssigned, settings.isAnnotationItalic);
    ::SendMessage(handle, SCI_STYLESETBOLD, styleAssigned, settings.isAnnotationBold);

    if (settings.enableAnnotation) {
      showAnnotations(handle);
    } else {
      hideAnnotations(handle);
    }
  }

  void ErrorAnnotator::drawAnnotations(HWND handle, const LineError& lineError) const {
    ::SendMessage(handle, SCI_ANNOTATIONSETTEXT, lineError.line, reinterpret_cast<LPARAM>(lineError.message.c_str()));
    ::SendMessage(handle, SCI_ANNOTATIONSETSTYLE, lineError.line, 0); // Use the first (and the only) style assigned to us
  }

  // Since indication locations are not tracked after they were draw, calling this methid could cause newly rendered incations to be off
  void ErrorAnnotator::changeIndicator(int indicator) {
    // Clear indications from both views if they are Papyrus scripts
    std::wstring mainViewFilePath = getApplicableFilePathOnView(MAIN_VIEW);
    if (!mainViewFilePath.empty()) {
      clearIndications(nppData._scintillaMainHandle);
    }
    std::wstring secondViewFilePath = getApplicableFilePathOnView(SUB_VIEW);
    if (!secondViewFilePath.empty()) {
      clearIndications(nppData._scintillaSecondHandle);
    }

    // Draw new indications if needed
    indicatorID = indicator;
    if (!mainViewFilePath.empty()) {
      updateIndicatorStyleOnFile(nppData._scintillaMainHandle, mainViewFilePath);
    }
    if (!secondViewFilePath.empty()) {
      updateIndicatorStyleOnFile(nppData._scintillaSecondHandle, secondViewFilePath);
    }
  }

  void ErrorAnnotator::updateIndicatorStyle() {
    // Update indicator style of the current file on the given view if it's Papyrus script
    if (!getApplicableFilePathOnView(MAIN_VIEW).empty()) {
      updateIndicatorStyle(nppData._scintillaMainHandle);
    }
    if (!getApplicableFilePathOnView(SUB_VIEW).empty()) {
        updateIndicatorStyle(nppData._scintillaSecondHandle);
    }
  }

  void ErrorAnnotator::updateIndicatorStyle(HWND handle) const {
    ::SendMessage(handle, SCI_INDICSETFORE, indicatorID, settings.indicatorForegroundColor);
    ::SendMessage(handle, SCI_SETINDICATORCURRENT, indicatorID, 0);

    if (settings.enableIndication) {
      showIndications(handle);
    } else {
      hideIndications(handle);
    }
  }

  void ErrorAnnotator::updateIndicatorStyleOnFile(HWND handle, const std::wstring& filePath) {
    // Check if current file has errors
    auto fileErrors = errors.find(utility::toUpper(filePath));
    if (fileErrors != errors.end()) {
      updateIndicatorStyle(handle);
      for (const LineError& lineError : fileErrors->second) {
        drawIndications(handle, lineError);
      }
    }
  }

  void ErrorAnnotator::drawIndications(HWND handle, const LineError& lineError) const {
    // Get line start position and length
    npp_position_t lineStart = ::SendMessage(handle, SCI_POSITIONFROMLINE, lineError.line, 0);
    npp_position_t lineLength = ::SendMessage(handle, SCI_LINELENGTH, lineError.line, 0);
    
    // Scintilla does not use wide char, also returned line length does not include the ending null char
    char* line = new char[lineLength + 1];
    auto autoCleanup = gsl::finally([&] { delete[] line; });
    npp_position_t filledLength = ::SendMessage(handle, SCI_GETLINE, lineError.line, reinterpret_cast<LPARAM>(line));
    if (filledLength <= lineLength) {
      line[filledLength] = 0;
    }

    for (int column : lineError.columns) {
      int length = 0;
      int search = column;
      if (search < filledLength) {
        if (isalpha(line[search]) || line[search] == '_') {
          // Papyrus keyword starts with alpha or underscore and can have numbers in it
          while (search < filledLength
            && (isalnum(line[search]) || line[search] == '_')
          ) {
            search++;
            length++;
          }
        } else if (isdigit(line[search]) || line[search] == '-') {
          // Numbers are digits or hex numbers starting with 0[xX]
          bool isHex = (line[search] == '0' && filledLength - search > 1 && tolower(line[search + 1]) == 'x');
          bool hasDigit = false;
          while (search < filledLength
            && (isdigit(line[search])
              || (line[search] == '-' && search == column) // leading minus sign
              || (line[search] == '.' && hasDigit) // decimal point after at least a digit
              || (isHex && (isxdigit(line[search]) || (tolower(line[search]) == 'x' && search == column + 1)))
            )
          ) {
            if (isdigit(line[search])) {
              hasDigit = true;
            }
            search++;
            length++;
          }
        } else if (!isblank(line[search]) && line[search] != '\r' && line[search] != '\n') {
          while (search < filledLength
            && !isalnum(line[search]) && !isblank(line[search]) && line[search] != '\r' && line[search] != '\n') {
            search++;
            length++;
          }
        } else {
          // Blank or line end
          length = 1;
        }
      }

      ::SendMessage(handle, SCI_INDICATORFILLRANGE, lineStart + column, length);
    }
  }

} // namespace
