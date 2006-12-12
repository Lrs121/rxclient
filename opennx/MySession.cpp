// $Id$
//
// Copyright (C) 2006 The OpenNX Team
// Author: Fritz Elfert
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU Library General Public License as
// published by the Free Software Foundation; either version 2 of the
// License, or (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Library General Public
// License along with this program; if not, write to the
// Free Software Foundation, Inc.,
// 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
//

#if defined(__GNUG__) && !defined(__APPLE__)
#pragma implementation "SessionList.h"
#endif

// For compilers that support precompilation, includes "wx/wx.h".
#include "wx/wxprec.h"

#ifdef __BORLANDC__
#pragma hdrstop
#endif

#ifndef WX_PRECOMP
#include "wx/wx.h"
#endif

#include "ConnectDialog.h"
#include "MySession.h"
#include "MyXmlConfig.h"
#include "MyIPC.h"

#include <wx/filename.h>
#include <wx/regex.h>
#include <wx/wfstream.h>
#include <wx/txtstrm.h>
#include <wx/socket.h>
#include <wx/config.h>
//#include <wx/dynlib.h>
#include <wx/utils.h>
#include <wx/tokenzr.h>

#ifdef MYTRACETAG
# undef MYTRACETAG
#endif
#define MYTRACETAG wxT("MySession")

#define NX_PROTOCOL_VERSION wxT("2.1.0")

IMPLEMENT_CLASS(MySession, wxEvtHandler);

BEGIN_EVENT_TABLE(MySession, wxEvtHandler)
    EVT_COMMAND(wxID_ANY, wxEVT_NXSSH, MySession::OnSshEvent)
END_EVENT_TABLE();

MySession::MySession(wxString dir, wxString status, wxString stype, wxString host, int port, wxString md5)
    : m_sHost(host)
    , m_iPort(port)
    , m_lPid(0)
    , m_sMd5(md5)
    , m_eSessionType(None)
    , m_eSessionStatus(Unknown)
    , m_sDir(dir)
    , m_bTouched(true)
    , m_pNxSsh(NULL)
    , m_pCfg(NULL)
    , m_pDlg(NULL)
{
    if (stype == wxT("C"))
        m_eSessionType = Server;
    if (stype == wxT("S"))
        m_eSessionType = Client;

    if (status == wxT("T"))
        m_eSessionStatus = Terminated;
    if (status == wxT("F"))
        m_eSessionStatus = Failed;

    m_rePID.Compile(wxT("([[:digit:]]+)"));

    CheckState();
    bGetPidFromFile();
}

MySession::MySession(const MySession &src)
{
    m_sHost = src.m_sHost;
    m_iPort = src.m_iPort;
    m_lPid = src.m_lPid;
    m_sMd5 = src.m_sMd5;
    m_eSessionType = src.m_eSessionType;
    m_eSessionStatus = src.m_eSessionStatus;
    m_sDir = src.m_sDir;
    m_bTouched = src.m_bTouched;
    m_rePID.Compile(wxT("([[:digit:]]+)"));
}

MySession & MySession::operator =(const MySession &src)
{
    m_sHost = src.m_sHost;
    m_iPort = src.m_iPort;
    m_lPid = src.m_lPid;
    m_sMd5 = src.m_sMd5;
    m_eSessionType = src.m_eSessionType;
    m_eSessionStatus = src.m_eSessionStatus;
    m_sDir = src.m_sDir;
    m_bTouched = src.m_bTouched;
    m_rePID.Compile(wxT("([[:digit:]]+)"));
    return *this;
}

MySession::~MySession()
{
    ::wxLogTrace(MYTRACETAG, wxT("~MySession: md5='%s'"), m_sMd5.c_str());
}

    wxString
MySession::sGetCreationTime()
{
    wxDateTime ctime;
    wxFileName fn(m_sDir, wxT("session"));
    fn.GetTimes(NULL, NULL, &ctime);
    return ctime.Format();
}

    bool
MySession::bGetPidFromFile()
{
    wxFileInputStream input(m_sDir + wxFileName::GetPathSeparator() + wxT("session"));
    wxTextInputStream text(input);
    int cnt = 0;

    m_lPid = 0;
    while ((!input.Eof()) && (cnt < 100)) {
        wxString line = text.ReadLine();
        int idx = line.Find(wxT("pid"));

        if (idx != -1 &&
                line.Find(wxT("NXAGENT")) == -1 &&
                line.Find(wxT("Agent")) == -1)
        {
            line = line.Mid(idx + 4);
            if (m_rePID.Matches(line))
                m_rePID.GetMatch(line,1).ToLong(&m_lPid);
        }
        cnt++;
    }
    return (m_lPid > 0);
}

    void
MySession::CheckState()
{
    if ((m_eSessionStatus == Terminated) || (m_eSessionStatus == Failed))
        return;

    wxFFile fi(m_sDir + wxFileName::GetPathSeparator() + wxT("session"));
    wxFFileInputStream input(fi);

    if (fi.Length() > 2048)
        input.SeekI(2048, wxFromEnd);
    wxTextInputStream text(input);
    while (!input.Eof()) {
        wxString line = text.ReadLine();
        if (line.IsEmpty())
            continue;
        if (line.Find(wxT("Info: Shutting down the link and exiting")) != -1) {
            m_eSessionStatus = Terminated;
            break;
        }

    }
}

    int
MySession::getFirstFreePort()
{
    int port = 7000;
    wxIPV4address a;

    a.LocalHost();
    while (port < 65535) {
        a.Service(port);
        wxSocketServer ss(a);
        if (ss.Ok())
            return port;
    }
    return 0;
}

    wxString
MySession::getXauthCookie()
{
#ifdef __UNIX__
    wxString dpy;
    if (wxGetEnv(wxT("DISPLAY"), &dpy) && (!dpy.IsEmpty())) {
        wxString cmd = wxT("xauth list :");
        cmd << dpy.AfterFirst(wxT(':'));
        wxArrayString clines;
        if (::wxExecute(cmd, clines) == 0) {
            size_t count = clines.GetCount();
            for (size_t i = 0; i < count; i++) {
                wxString line = clines[i];
                if (line.Contains(wxT("MIT-MAGIC-COOKIE-1"))) {
                    wxStringTokenizer t(line, wxT("\t "));
                    int ecount = 0;
                    while (t.HasMoreTokens()) {
                        wxString tok = t.GetNextToken();
                        if (++ecount == 3)
                            return tok;
                    }
                }
            }
        }
    }
#endif
    return wxString();
}

    void
MySession::OnSshEvent(wxCommandEvent &event)
{
    MyIPC::tSessionEvents e = wx_static_cast(MyIPC::tSessionEvents, event.GetInt());
    wxString msg(event.GetString());
    wxString scmd;

    switch (e) {
        case MyIPC::ActionStatus:
            m_pDlg->SetStatusText(msg);
            break;
        case MyIPC::ActionLog:
            m_iProgress++;
            m_pDlg->SetProgress(m_iProgress);
            // TODO implement logging to runlog
            break;
        case MyIPC::ActionWarning:
            ::wxLogWarning(msg);
            break;
        case MyIPC::ActionError:
            ::wxLogError(msg);
            m_bGotError = true;
            break;
        case MyIPC::ActionPromptYesNo:
            if (wxMessageBox(msg, _("Warning"), wxYES_NO|wxICON_EXCLAMATION) == wxYES)
                m_pNxSsh->Print(wxT("yes"));
            else
                m_pNxSsh->Print(wxT("no"));
            break;
        case MyIPC::ActionSendUsername:
            m_pDlg->SetStatusText(_("Sending username"));
            m_pNxSsh->Print(m_pCfg->sGetUsername());
            break;
        case MyIPC::ActionSendPassword:
            m_pDlg->SetStatusText(_("Authenticating"));
            if (m_pCfg->bGetRememberPassword()) {
                m_pNxSsh->Print(m_pCfg->sGetDecryptedPassword());
            } else {
                m_pNxSsh->Print(m_sClearPassword);
            }
            break;
        case MyIPC::ActionWelcome:
            m_pDlg->SetStatusText(_("Authentication successful"));
            m_eConnectState = STATE_START_SESSION;
            break;
        case MyIPC::ActionNextCommand:
            m_iProgress += 5;
            m_pDlg->SetProgress(m_iProgress);
            switch (m_eConnectState) {
                case STATE_HELLO:
                    m_pNxSsh->Print(wxT("hello NXCLIENT - Version ") NX_PROTOCOL_VERSION);
                    m_eConnectState = STATE_SHELLMODE;
                    break;
                case STATE_SHELLMODE:
                    m_pNxSsh->Print(wxT("SET SHELL_MODE SHELL"));
                    m_eConnectState = STATE_AUTHMODE;
                    break;
                case STATE_AUTHMODE:
                    m_pNxSsh->Print(wxT("SET AUTH_MODE PASSWORD"));
                    m_eConnectState = STATE_LOGIN;
                    break;
                case STATE_LOGIN:
                    m_pNxSsh->Print(wxT("login"));
                    break;
                case STATE_START_SESSION:
                    scmd = wxT("startsession ");
                    scmd << m_pCfg->sGetSessionParams(NX_PROTOCOL_VERSION);
#ifdef __UNIX__
                    if (!m_sXauthCookie.IsEmpty())
                        scmd << wxT(" --cookie=\"%s\"") << m_sXauthCookie;
#endif
                    m_pNxSsh->Print(scmd);
                    m_eConnectState = STATE_FINISH;
                    break;
                case STATE_FINISH:
                    m_pNxSsh->Print(wxT(""));
                    break;
            }
            break;
        case MyIPC::ActionPassphraseDialog:
            ::wxMessageBox(wxT("NOT IMPLMENTED"),
                    wxT("iMyIPC::ActionPassphraseDialog"));
            m_pNxSsh->Print(wxT("notimplemented"));
            break;
        case MyIPC::ActionSetSessionID:
            m_pDlg->SetStatusText(_("Negotiating session parameter"));
            m_sSessionID = msg;
            break;
        case MyIPC::ActionSetProxyCookie:
            m_pDlg->SetStatusText(_("Negotiating session parameter"));
            m_sProxyCookie = msg;
            break;
        case MyIPC::ActionSetProxyIP:
            m_pDlg->SetStatusText(_("Negotiating session parameter"));
            m_sProxyIP = msg;
            break;
        case MyIPC::ActionSetSessionType:
            m_pDlg->SetStatusText(_("Negotiating session parameter"));
            m_sSessionType = msg;
            break;
        case MyIPC::ActionSetSessionCache:
            m_pDlg->SetStatusText(_("Negotiating session parameter"));
            m_sSessionCache = msg;
            break;
        case MyIPC::ActionSetSessionDisplay:
            m_pDlg->SetStatusText(_("Negotiating session parameter"));
            m_sSessionDisplay = msg;
            break;
        case MyIPC::ActionSetAgentCookie:
            m_pDlg->SetStatusText(_("Negotiating session parameter"));
            m_sAgentCookie = msg;
            break;
        case MyIPC::ActionSetSslTunneling:
            m_pDlg->SetStatusText(_("Negotiating session parameter"));
            m_bSslTunneling = (msg == wxT("1"));
            break;
        case MyIPC::ActionSetSubscription:
            m_pDlg->SetStatusText(_("Negotiating session parameter"));
            m_sSubscription = msg;
            break;
        case MyIPC::ActionExit:
            m_pDlg->SetStatusText(_("Starting session"));
            msg = wxT("NX> 299 Switch connection to: ");
            msg << m_sProxyIP << wxT(":") << m_sProxyPort
                << wxT(" cookie: ") << m_sProxyCookie;
            m_pNxSsh->Print(msg);
            break;
        case MyIPC::ActionStartProxy:
            {
                wxString popts;
                popts << wxT("nx,cookie=") << m_sProxyCookie
                    << wxT(",root=") << m_sUserDir
                    << m_pCfg->sGetProxyParams(NX_PROTOCOL_VERSION)
                    << wxT(",product=") << m_sSubscription
                    << wxT(",encryption=") << (m_bSslTunneling ? 1 : 0)
                    << wxT(",session=session")
#ifdef __UNIX__
                    << wxT(",client=linux")
#else
                    << wxT(",client=win")
#endif
                    << wxT(",id=") << m_sSessionID;
                if (m_bSslTunneling) {
                    m_sProxyIP = wxT("127.0.0.1");
                    m_sProxyPort = wxString::Format(wxT("%d"), getFirstFreePort());
                    popts << wxT(",listen=") << m_sProxyPort
                        << wxT(":") << m_sSessionDisplay;
                } else {
                    popts << wxT(",connect=") << m_sProxyIP
                        << wxT(":") << m_sSessionDisplay;
                }

                m_sOptFilename = m_sUserDir;
                m_sOptFilename << wxFileName::GetPathSeparator()
                    << wxT("S-") << m_sSessionID;
                {
                    wxFileName::Mkdir(m_sOptFilename, 0700, wxPATH_MKDIR_FULL);
                    // TODO: error handling
                    m_sOptFilename << wxFileName::GetPathSeparator() << wxT("options");
                    wxFile f;
                    if (f.Open(m_sOptFilename, wxFile::write_excl, wxS_IRUSR|wxS_IWUSR)) {
                        f.Write(popts + wxT("\n"));
                        f.Close();

                        ::wxLogDebug(wxT("opts='%s'\n"), popts.c_str());
                        wxString pcmd;
                        wxConfigBase::Get()->Read(wxT("Config/SystemNxDir"), &pcmd);
                        pcmd << wxFileName::GetPathSeparator() << wxT("bin")
                            << wxFileName::GetPathSeparator() << wxT("nxproxy -S nx,options=")
                            << m_sOptFilename << wxT(":") << m_sSessionDisplay;
                        m_pNxSsh->Print(wxT("bye"));
                        ::wxExecute(pcmd);
                    }
                }
            }
            break;
        case MyIPC::ActionSessionRunning:
            m_bSessionRunning = true;
            break;
    }
}

bool MySession::Create(const wxString cfgFileName, const wxString password)
{
    m_sClearPassword = password;
    m_bSessionRunning = false;
    MyXmlConfig cfg(cfgFileName);
    m_pCfg = &cfg;
    if (cfg.IsValid()) {
        wxString sysDir;
        wxString userDir;
        wxConfigBase::Get()->Read(wxT("Config/SystemNxDir"), &sysDir);
        wxConfigBase::Get()->Read(wxT("Config/UserNxDir"), &userDir);
        m_sUserDir = userDir;
        wxFileName fn(sysDir, wxT(""));

        fn.AppendDir(wxT("bin"));
        fn.SetName(wxT("nxssh"));
        wxString nxsshcmd = fn.GetShortPath();
        nxsshcmd << wxT(" -nx -x -2")
            << wxT(" -p ") << cfg.iGetServerPort()
            << wxT(" -o 'RhostsAuthentication no'")
            << wxT(" -o 'PasswordAuthentication no'")
            << wxT(" -o 'RSAAuthentication no'")
            << wxT(" -o 'RhostsRSAAuthentication no'")
            << wxT(" -o 'PubkeyAuthentication yes'");

        wxString tmpDir = userDir;
        tmpDir << wxFileName::GetPathSeparator() << wxT("temp")
            << wxFileName::GetPathSeparator() << ::wxGetProcessId();
        wxFileName::Mkdir(tmpDir, 0700, wxPATH_MKDIR_FULL);

        if (cfg.sGetSshKey().IsEmpty()) {
            fn.Assign(sysDir, wxT("server.id_dsa.key"));
            fn.AppendDir(wxT("share"));
            fn.AppendDir(wxT("keys"));
        } else {
            fn.Assign(tmpDir, wxT("keylog"));
            wxFile f;
            if (f.Open(fn.GetFullPath(), wxFile::write_excl, wxS_IRUSR|wxS_IWUSR)) {
                f.Write(cfg.sGetSshKey());
                f.Close();
            } else {
                wxLogSysError(_("Could not write %s"), fn.GetFullPath().c_str());
                return false;
            }
        }

        nxsshcmd << wxT(" -i ") << fn.GetShortPath();
        if (cfg.bGetEnableSSL())
            nxsshcmd << wxT(" -B");
        nxsshcmd << wxT(" -E") << wxT(" nx@") << cfg.sGetServerHost();

        ::wxSetEnv(wxT("NX_HOME"), wxFileName::GetHomeDir());
        ::wxSetEnv(wxT("NX_ROOT"), userDir);
        ::wxSetEnv(wxT("NX_SYSTEM"), sysDir);
        ::wxSetEnv(wxT("NX_CLIENT"), wxT("/usr/NX/bin/nxclient"));
        //        wxT("/home/felfert/Projects/NX/mxclient/opennx"));
        ::wxSetEnv(wxT("NX_VERSION"), NX_PROTOCOL_VERSION);
        ::wxSetEnv(wxT("XAUTHORITY"), wxFileName::GetHomeDir() + wxFileName::GetPathSeparator() + wxT(".Xauthority"));
        // FIXME: use the dir containing .X11-unix, since nxproxy assumes that
        ::wxSetEnv(wxT("NX_TEMP"), wxT("/tmp"));

#if 0
        int proxyPort = -1;
        if (cfg.bGetUseSmartCard())
            cmd += wxT("-I 0 ");
        else
            cmd += wxString::Format(wxT("-i %s/share/keys/server.id_dsa.key "), fn.GetShortPath().c_str());
        if (cfg.bGetEnableSSL()) {
            proxyPort = getFirstFreePort();
            cmd += wxString::Format(wxT("-D %d "), proxyPort);
        }
#endif

        m_sXauthCookie = getXauthCookie();
        fn.Assign(sysDir, wxT("bin"));
        m_iProgress = 0;
        ConnectDialog dlg(NULL);
        m_pDlg = &dlg;
        dlg.SetStatusText(wxString::Format(wxT("Connecting to %s ..."),
                    cfg.sGetServerHost().c_str()));
        dlg.Show(true);
        MyIPC nxssh;
        m_pNxSsh = &nxssh;
        if (nxssh.SshProcess(nxsshcmd, fn.GetShortPath(), this)) {
            m_bGotError = false;
            m_eConnectState = STATE_HELLO;
            while (!(dlg.bGetAbort() || m_bGotError || m_bSessionRunning)) {
                dlg.Update();
                wxSafeYield(&dlg);
            }
            if (dlg.bGetAbort() || m_bGotError) {
                nxssh.Kill();
                return false;
            }
        } else {
            ::wxLogError(_("Called command was: ") + nxsshcmd);
            ::wxLogError(_("Could not start nxssh."));
            return false;
        }
    }
    return true;
}
