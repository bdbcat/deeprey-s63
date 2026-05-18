/******************************************************************************
 *
 * Project:  OpenCPN / Deeprey
 * Purpose:  Deeprey S63 API implementation
 *
 * Implements DpS63::DpS63API by delegating to the s63_pi plugin. deeprey-gui
 * receives the DpS63API pointer via SendPluginMessage("S63_API_TO_DP_GUI", ...)
 * and drives S63 chart management through this surface.
 *
 ***************************************************************************
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 ***************************************************************************
 */

#include "wx/wxprec.h"
#ifndef WX_PRECOMP
#include "wx/wx.h"
#endif

#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/textfile.h>
#include <wx/tokenzr.h>

#include "s63_pi.h"
#include "DpS63API.h"

//  Globals owned by s63_pi.cpp.
extern wxString g_userpermit;
extern wxString g_installpermit;
extern wxString g_fpr_file;
extern wxString g_SENCdir;

namespace DpS63 {

DpS63API::DpS63API(s63_pi* plugin) : m_plugin(plugin) {}

DpS63API::~DpS63API() {}

// ---------------------------------------------------------------------------
//  Storage locations
// ---------------------------------------------------------------------------

wxString DpS63API::GetChartDir() {
    return m_plugin ? m_plugin->GetPermitDir() : wxString();
}

wxString DpS63API::GetCertificateDir() {
    return m_plugin ? m_plugin->GetCertificateDir() : wxString();
}

wxString DpS63API::GetSencDir() {
    return g_SENCdir;
}

// ---------------------------------------------------------------------------
//  Installed cell enumeration
// ---------------------------------------------------------------------------

// Parse a single .os63 permit file into a DpS63CellInfo. Mirrors the cellpermit
// parsing in OCPNPermitList::BuildList(): the permit string carries the cell
// name (chars 0-7) and the ENC expiry date (chars 8-15, %Y%m%d); the line tail
// after ':' is comma-separated permit,serviceLevel,edition,dataServerID.
static bool ParseOs63File(const wxString& path, DpS63CellInfo& out) {
    wxTextFile file(path);
    if (!file.Open()) return false;

    bool found = false;
    for (wxString line = file.GetFirstLine(); !file.Eof();
         line = file.GetNextLine()) {
        if (!line.StartsWith(_T("cellpermit"))) continue;

        wxString permit_string = line.Mid(11);
        out.cellName = permit_string.Mid(0, 8);

        wxString sdate = permit_string.Mid(8, 8);
        out.expiryDate.ParseFormat(sdate, _T("%Y%m%d"));

        wxStringTokenizer tkz(line.AfterFirst(':'), _T(","));
        tkz.GetNextToken();                       // permit
        tkz.GetNextToken();                       // service level
        out.edition = tkz.GetNextToken();         // edition
        out.producer = tkz.GetNextToken();        // data server ID

        out.permitFile = path;
        out.sizeBytes = wxFileName::GetSize(path).GetValue();

        if (out.expiryDate.IsValid() && out.expiryDate < wxDateTime::Now())
            out.status = DpS63CellStatus::PERMIT_EXPIRED;
        else
            out.status = DpS63CellStatus::INSTALLED;

        found = true;
        break;
    }
    return found;
}

std::vector<DpS63CellInfo> DpS63API::GetInstalledCells() {
    std::vector<DpS63CellInfo> cells;
    wxString dir = GetChartDir();
    if (dir.IsEmpty() || !wxDir::Exists(dir)) return cells;

    wxArrayString files;
    wxDir::GetAllFiles(dir, &files, _T("*.os63"));
    for (const wxString& f : files) {
        DpS63CellInfo info;
        if (ParseOs63File(f, info)) cells.push_back(info);
    }
    return cells;
}

DpS63CellInfo DpS63API::GetCellInfo(const wxString& cellName) {
    for (const DpS63CellInfo& c : GetInstalledCells()) {
        if (c.cellName.IsSameAs(cellName, false)) return c;
    }
    DpS63CellInfo empty;
    empty.cellName = cellName;
    empty.status = DpS63CellStatus::NOT_INSTALLED;
    return empty;
}

bool DpS63API::RemoveCell(const wxString& cellName) {
    DpS63CellInfo info = GetCellInfo(cellName);
    if (info.permitFile.IsEmpty() || !wxFileExists(info.permitFile))
        return false;

    RemoveChartFromDBInPlace(info.permitFile);
    bool ok = ::wxRemoveFile(info.permitFile);
    if (ok) NotifyStateChanged();
    return ok;
}

// ---------------------------------------------------------------------------
//  Certificate management
// ---------------------------------------------------------------------------

std::vector<DpS63CertificateInfo> DpS63API::GetCertificates() {
    std::vector<DpS63CertificateInfo> certs;
    wxString dir = GetCertificateDir();
    if (dir.IsEmpty() || !wxDir::Exists(dir)) return certs;

    wxArrayString files;
    wxDir::GetAllFiles(dir, &files, _T("*.PUB"));
    for (const wxString& f : files) {
        DpS63CertificateInfo info;
        wxFileName fn(f);
        info.name = fn.GetFullName();
        info.path = f;
        info.isDefaultIHO = info.name.IsSameAs(_T("IHO.PUB"), false);
        certs.push_back(info);
    }
    return certs;
}

bool DpS63API::ImportCertificate(const wxString& pubFilePath) {
    if (!m_plugin || !wxFileExists(pubFilePath)) return false;

    m_plugin->m_apiCertFileOverride = pubFilePath;
    int rv = m_plugin->ImportCert();
    m_plugin->m_apiCertFileOverride.Clear();

    if (rv == 0) NotifyStateChanged();
    return rv == 0;
}

bool DpS63API::RemoveCertificate(const wxString& certName) {
    wxString dir = GetCertificateDir();
    if (dir.IsEmpty()) return false;

    wxString path = dir + wxFileName::GetPathSeparator() + certName;
    if (!wxFileExists(path)) return false;

    bool ok = ::wxRemoveFile(path);
    if (ok) NotifyStateChanged();
    return ok;
}

// ---------------------------------------------------------------------------
//  Permit / hardware identity
// ---------------------------------------------------------------------------

DpS63PermitStatus DpS63API::GetPermitStatus() {
    DpS63PermitStatus status;
    status.userpermit = g_userpermit;
    status.installpermit = g_installpermit;
    status.fingerprint = g_fpr_file;
    status.hasUserpermit =
        g_userpermit.Len() && !g_userpermit.IsSameAs(_T("X"));
    status.hasInstallpermit =
        g_installpermit.Len() && !g_installpermit.IsSameAs(_T("Y"));
    return status;
}

bool DpS63API::SetUserpermit(const wxString& userpermit) {
    if (!m_plugin) return false;
    g_userpermit = userpermit;
    m_plugin->SaveConfig();
    NotifyStateChanged();
    return true;
}

bool DpS63API::SetInstallpermit(const wxString& installpermit) {
    if (!m_plugin) return false;
    g_installpermit = installpermit;
    m_plugin->SaveConfig();
    NotifyStateChanged();
    return true;
}

wxString DpS63API::CreateFingerprintFile(const wxString& targetDir) {
    wxString dir = targetDir;
    if (dir.IsEmpty()) return wxString();
    if (dir.Last() != wxFileName::GetPathSeparator())
        dir += wxFileName::GetPathSeparator();

    //  Invoke OCPNsenc to write a fingerprint (XFPR) file. Same command the
    //  native OnNewFPRClick handler uses, minus the confirmation dialogs.
    wxString cmd = _T(" -w -o ") + wxString('\"') + dir + wxString('\"');
    wxArrayString result = exec_SENCutil_sync(cmd, false);

    wxString fpr_file;
    bool err = false;
    for (const wxString& line : result) {
        if (line.Upper().Find(_T("ERROR")) != wxNOT_FOUND) {
            err = true;
            break;
        }
        if (line.Upper().Find(_T("FPR")) != wxNOT_FOUND)
            fpr_file = line.AfterFirst(':').Trim().Trim(false);
    }

    if (err || fpr_file.IsEmpty()) return wxString();

    g_fpr_file = fpr_file;
    if (m_plugin) {
        m_plugin->Set_FPR();
        m_plugin->SaveConfig();
    }
    NotifyStateChanged();
    return fpr_file;
}

// ---------------------------------------------------------------------------
//  Import (offline, file-based)
// ---------------------------------------------------------------------------

void DpS63API::ImportPermitFile(const wxString& permitFilePath,
                                CompleteCallback onComplete) {
    if (!m_plugin || !wxFileExists(permitFilePath)) {
        if (onComplete)
            onComplete(DpS63ImportResult::BAD_PERMIT_FILE,
                       _("Permit file not found"));
        return;
    }

    m_plugin->m_apiPermitFileOverride = permitFilePath;
    int rv = m_plugin->ImportCellPermits();
    m_plugin->m_apiPermitFileOverride.Clear();

    DpS63ImportResult result =
        (rv == 0) ? DpS63ImportResult::SUCCESS : DpS63ImportResult::NO_USERPERMIT;
    NotifyStateChanged();
    if (onComplete)
        onComplete(result, rv == 0 ? _("Permits imported")
                                   : _("Userpermit/Installpermit required"));
}

void DpS63API::ImportCells(const wxString& cellSourceDir,
                           ProgressCallback onProgress,
                           CompleteCallback onComplete) {
    (void)onProgress;  // s63_pi drives its own wxProgressDialog during import
    if (!m_plugin || !wxDir::Exists(cellSourceDir)) {
        if (onComplete)
            onComplete(DpS63ImportResult::UNKNOWN_ERROR,
                       _("Cell source directory not found"));
        return;
    }

    m_plugin->m_apiEncRootOverride = cellSourceDir;
    int rv = m_plugin->ImportCells();
    m_plugin->m_apiEncRootOverride.Clear();

    DpS63ImportResult result =
        (rv == 0) ? DpS63ImportResult::SUCCESS : DpS63ImportResult::SENC_BUILD_FAILED;
    NotifyStateChanged();
    if (onComplete)
        onComplete(result, rv == 0 ? _("Cells imported")
                                   : _("Cell import failed"));
}

// ---------------------------------------------------------------------------
//  State-change notification
// ---------------------------------------------------------------------------

uint64_t DpS63API::AddStateChangedCallback(std::function<void()> callback) {
    uint64_t id = m_nextCallbackId++;
    m_stateCallbacks[id] = std::move(callback);
    return id;
}

void DpS63API::RemoveStateChangedCallback(uint64_t callbackId) {
    m_stateCallbacks.erase(callbackId);
}

void DpS63API::NotifyStateChanged() {
    for (auto& kv : m_stateCallbacks) {
        if (kv.second) kv.second();
    }
}

}  // namespace DpS63
