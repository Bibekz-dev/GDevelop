/** \file
 *  Game Develop
 *  2008-2012 Florian Rival (Florian.Rival@gmail.com)
 */

#if defined(GD_IDE_ONLY)

#include "FullProjectCompiler.h"
#include <fstream>
#include <iostream>
#include <wx/filefn.h>
#include <wx/log.h>
#include <wx/msgdlg.h>
#include <wx/stopwatch.h>
#include <wx/dir.h>
#include <wx/help.h>
#include <wx/config.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/filename.h>
#include "GDCore/PlatformDefinition/ExternalEvents.h"
#include "GDL/IDE/CodeCompiler.h"
#include "GDL/Events/CodeCompilationHelpers.h"
#include "GDL/DatFile.h"
#include "GDL/Game.h"
#include "GDL/Scene.h"
#include "GDL/Object.h"
#include "GDL/OpenSaveLoadingScreen.h"
#include "GDL/SceneNameMangler.h"
#include "GDL/Tools/AES.h"
#include "GDL/CommonTools.h"
#include "GDL/ExtensionsManager.h"
#include "GDL/ExtensionBase.h"
#include "GDL/ExternalEvents.h"
#include "GDL/OpenSaveGame.h"
#include "GDCore/IDE/ResourcesMergingHelper.h"
#include "GDL/IDE/ExecutableIconChanger.h"
#include "GDL/IDE/BaseProfiler.h"
#include "PlatformDefinition/Platform.h"

using namespace std;

namespace GDpriv
{

void FullProjectCompiler::LaunchProjectCompilation()
{
    #if defined(WINDOWS)
        windowsTarget = true;
        linuxTarget = false;
        macTarget = false;
    #elif defined(LINUX)
        windowsTarget = false;
        linuxTarget = true;
        macTarget = false;
        compressIfPossible = false;
    #elif defined(MAC)
        windowsTarget = false;
        linuxTarget = true;
        macTarget = false;
        compressIfPossible = false;
    #else
        #warning Unknown OS
    #endif

    std::string winExecutableName = gameToCompile.winExecutableFilename.empty() ? "GameWin.exe" : gameToCompile.winExecutableFilename+".exe";
    std::string linuxExecutableName = gameToCompile.linuxExecutableFilename.empty() ? "GameLinux" : gameToCompile.linuxExecutableFilename;
    std::string macExecutableName = gameToCompile.macExecutableFilename.empty() ? "GameMac" : gameToCompile.macExecutableFilename;

    diagnosticManager.OnMessage(ToString(_("Lancement de la compilation du projet.")));
    if ( !windowsTarget && !linuxTarget && !macTarget)
    {
        diagnosticManager.AddError(ToString(_("Pas de syst�me cible d�fini.")));
        diagnosticManager.OnCompilationFailed();
        return;
    }

    //Used to handle all files which must be exported
    gd::ResourcesMergingHelper resourcesMergingHelper;

    wxLogNull noLogPlease;
    wxString tempDir = GetTempDir();
    ClearDirectory(ToString(tempDir)); //Pr�paration du r�pertoire

    //Wait current compilations to end
    if ( CodeCompiler::GetInstance()->CompilationInProcess() )
    {
        diagnosticManager.OnMessage(ToString(_("Compilation en attente de la fin des t�ches en cours...")));

        wxStopWatch yieldClock;
        while (CodeCompiler::GetInstance()->CompilationInProcess())
        {
            if ( yieldClock.Time() > 50 )
            {
                wxSafeYield(NULL, true);
                yieldClock.Start();
            }
        }
    }

    //Create a separate copy of the game in memory, as we're going to apply it some modifications ( i.e changing resources path )
    Game game = gameToCompile;

    //Prepare resources to copy
    diagnosticManager.OnMessage( ToString( _("Pr�paration des ressources...") ));

    //Add images
    for ( unsigned int i = 0;i < game.resourceManager.resources.size() ;i++ )
    {
        if ( game.resourceManager.resources[i] == boost::shared_ptr<Resource>() )
            continue;

        diagnosticManager.OnMessage( ToString(_("Pr�paration des ressources...")), game.resourceManager.resources[i]->name );

        if ( game.resourceManager.resources[i]->UseFile() )
            resourcesMergingHelper.ExposeResource(game.resourceManager.resources[i]->GetFile());
    }
    if ( !game.loadingScreen.imageFichier.empty() )
        resourcesMergingHelper.ExposeResource( game.loadingScreen.imageFichier );

    //Add scenes resources
    for ( unsigned int i = 0;i < game.GetLayoutCount();i++ )
    {
        for (unsigned int j = 0;j<game.GetLayouts()[i]->GetInitialObjects().size();++j) //Add objects resources
        	game.GetLayouts()[i]->GetInitialObjects()[j]->ExposeResources(resourcesMergingHelper);

        LaunchResourceWorkerOnEvents(game, game.GetLayout(i).GetEvents(), resourcesMergingHelper);
    }
    //Add external events resources
    for ( unsigned int i = 0;i < game.GetExternalEventsCount();i++ )
    {
        LaunchResourceWorkerOnEvents(game, game.GetExternalEvents(i).GetEvents(), resourcesMergingHelper);
    }
    //Add global objects resources
    for (unsigned int j = 0;j<game.GetGlobalObjects().size();++j) //Add global objects resources
        game.GetGlobalObjects()[j]->ExposeResources(resourcesMergingHelper);

    //Compile all scene events to bitcode
    for (unsigned int i = 0;i<game.GetLayoutCount();++i)
    {
        if ( game.GetLayouts()[i]->profiler ) game.GetLayouts()[i]->profiler->profilingActivated = false;

        diagnosticManager.OnMessage(ToString(_("Compilation de la sc�ne ")+game.GetLayout(i).GetName()+_(".")));
        CodeCompilerTask task;
        task.compilationForRuntime = true;
        task.optimize = optimize;
        task.eventsGeneratedCode = true;
        task.inputFile = string(CodeCompiler::GetInstance()->GetWorkingDirectory()+ToString(game.GetLayouts()[i].get())+"events.cpp");
        task.outputFile = tempDir+"/GDpriv"+SceneNameMangler::GetMangledSceneName(game.GetLayouts()[i]->GetName())+".ir";
        task.preWork = boost::shared_ptr<CodeCompilerExtraWork>(new EventsCodeCompilerRuntimePreWork(&game, game.GetLayouts()[i].get(), resourcesMergingHelper));
        task.scene = game.GetLayouts()[i].get();

        CodeCompiler::GetInstance()->AddTask(task);

        wxStopWatch yieldClock;
        while (CodeCompiler::GetInstance()->CompilationInProcess())
        {
            if ( yieldClock.Time() > 50 )
            {
                wxSafeYield(NULL, true);
                yieldClock.Start();
            }
        }

        if ( !wxFileExists(task.outputFile) )
        {
            diagnosticManager.AddError(ToString(_("La compilation de la sc�ne ")+game.GetLayout(i).GetName()+_(" a �chou�e : Rendez vous sur notre site pour nous rapporter cette erreur, en joignant le fichier suivant:\n"+CodeCompiler::GetInstance()->GetWorkingDirectory()+"compilationErrors.txt"+"\n\nSi vous pensez que l'erreur provient d'une extension, contactez le d�veloppeur de celle ci.")));
            diagnosticManager.OnCompilationFailed();
            return;
        }
        else
            diagnosticManager.OnMessage(ToString(_("Compilation de la sc�ne ")+game.GetLayout(i).GetName()+_(" effectu�e avec succ�s.")));

        resourcesMergingHelper.ExposeResource(task.outputFile); //Export bitcode file.

        diagnosticManager.OnPercentUpdate( static_cast<float>(i) / static_cast<float>(game.GetLayoutCount())*50.0 );
    }

    //Now copy resources
    diagnosticManager.OnMessage( ToString( _("Copie des ressources...") ) );
    map<string, string> & resourcesNewFilename = resourcesMergingHelper.GetAllResourcesNewFilename();
    unsigned int i = 0;
    for(map<string, string>::const_iterator it = resourcesNewFilename.begin(); it != resourcesNewFilename.end(); ++it)
    {
        if ( !it->first.empty() && wxCopyFile( it->first, tempDir + "/" + it->second, true ) == false )
            diagnosticManager.AddError(ToString(_( "Impossible de copier " )+it->first+_(" dans le r�pertoire de compilation.\n" )));

        ++i;
        diagnosticManager.OnPercentUpdate( 50.0 + static_cast<float>(i) / static_cast<float>(resourcesNewFilename.size())*25.0 );
        wxSafeYield();
    }

    wxSafeYield();
    diagnosticManager.OnMessage(ToString(_( "Compilation du jeu..." )), ToString(_( "Etape 1 sur 3" )));
    OpenSaveGame saveGame( game );
    saveGame.SaveToFile(static_cast<string>( tempDir + "/compil.gdg" ));
    diagnosticManager.OnPercentUpdate(80);

    wxSafeYield();
    diagnosticManager.OnMessage(ToString(_( "Compilation du jeu..." )), ToString(_( "Etape 2 sur 3" )));

    //Cr�ation du fichier source
    {
        ifstream ifile(tempDir+"/compil.gdg",ios_base::binary);
        ofstream ofile(tempDir+"/src",ios_base::binary);

        // get file size
        ifile.seekg(0,ios_base::end);
        int size,fsize = ifile.tellg();
        ifile.seekg(0,ios_base::beg);

        // round up (ignore pad for here)
        size = (fsize+15)&(~15);

        char * ibuffer = new char[size];
        char * obuffer = new char[size];
        ifile.read(ibuffer,fsize);

        AES crypt;
        crypt.SetParameters(192);

        unsigned char key[] = "-P:j$4t&OHIUVM/Z+u4DeDP.";

        crypt.StartEncryption(key);
        crypt.Encrypt(reinterpret_cast<const unsigned char*>(ibuffer),reinterpret_cast<unsigned char*>(obuffer),size/16);

        ofile.write(obuffer,size);

        delete [] ibuffer;
        delete [] obuffer;

        ofile.close();
        ifile.close();
	}
    wxRemoveFile( tempDir + "/compil.gdg" );
    diagnosticManager.OnPercentUpdate(85);

    OpenSaveLoadingScreen saveLS(game.loadingScreen);
    saveLS.SaveToFile(string(tempDir + "/loadingscreen"));

    //Cr�ation du fichier gam.egd
    diagnosticManager.OnMessage(ToString(_( "Compilation du jeu..." )), ToString(_( "Etape 3 sur 3" )));
    wxSafeYield();

    //On cr�� une liste avec tous les fichiers
    vector < string > files;
    {
        wxString file = wxFindFirstFile( tempDir + "/*" );
        while ( !file.empty() )
        {
            wxFileName filename(file);

            files.push_back( static_cast<string>(filename.GetFullName()) );
            file = wxFindNextFile();
        }
    }

    //On cr�� le fichier � partir des fichiers
    DatFile gameDatFile;
    gameDatFile.Create(files, static_cast<string>(tempDir), static_cast<string>(tempDir + "/gam.egd"));

    //On supprime maintenant tout le superflu
    {
        wxString file = wxFindFirstFile( tempDir + "/*" );
        while ( !file.empty() )
        {
            wxFileName filename(file);
            if ( filename.GetFullName() != "gam.egd" ) //On supprime tout sauf gam.egd
            {
                if ( !wxRemoveFile( file ) )
                    diagnosticManager.AddError(ToString( _( "Impossible de supprimer le fichier " ) + file + _(" situ� dans le r�pertoire de compilation.\n" )));
            }

            file = wxFindNextFile();
        }
    }

    diagnosticManager.OnPercentUpdate(90);
    diagnosticManager.OnMessage(ToString(_( "Exportation du jeu..." )));
    wxSafeYield();

    //Copy extensions
    ExtensionsManager * extensionsManager = ExtensionsManager::GetInstance();
    for (unsigned int i = 0;i<game.GetUsedPlatformExtensions().size();++i)
    {
        //Builtin extensions does not have a namespace.
        boost::shared_ptr<ExtensionBase> extension = extensionsManager->GetExtension(game.GetUsedPlatformExtensions()[i]);

        if ( extension != boost::shared_ptr<ExtensionBase>() &&
            ( extension->GetNameSpace() != "" || extension->GetName() == "CommonDialogs" )
            && extension->GetName() != "BuiltinCommonInstructions" ) //Extension with a namespace but builtin
        {
            if ( windowsTarget)
            {
                if ( wxCopyFile( "Extensions/"+game.GetUsedPlatformExtensions()[i]+".xgdw", tempDir + "/" + game.GetUsedPlatformExtensions()[i]+".xgdw", true ) == false )
                    diagnosticManager.AddError(ToString(_( "Impossible de copier l'extension ")+game.GetUsedPlatformExtensions()[i]+_(" pour Windows dans le r�pertoire de compilation.\n" )));
            }

            if ( linuxTarget )
            {
                if ( wxCopyFile( "Extensions/"+game.GetUsedPlatformExtensions()[i]+".xgdl", tempDir + "/"+game.GetUsedPlatformExtensions()[i]+".xgdl", true ) == false )
                    diagnosticManager.AddError(ToString(_( "Impossible de copier l'extension ")+game.GetUsedPlatformExtensions()[i]+_(" pour Linux dans le r�pertoire de compilation.\n" )));
            }

            if ( macTarget )
            {
                if ( wxCopyFile( "Extensions/"+game.GetUsedPlatformExtensions()[i]+".xgdm", tempDir + "/"+game.GetUsedPlatformExtensions()[i]+".xgdm", true ) == false )
                    diagnosticManager.AddError(ToString(_( "Impossible de copier l'extension ")+game.GetUsedPlatformExtensions()[i]+_(" pour Mac OS dans le r�pertoire de compilation.\n" )));
            }
        }
        if ( extension != boost::shared_ptr<ExtensionBase>() )
        {
            if ( windowsTarget)
            {
                const std::vector < std::pair<std::string, std::string> > & supplementaryFiles = extension->GetSupplementaryRuntimeFiles();
                for (unsigned int i = 0;i<supplementaryFiles.size();++i)
                {
                    if ( supplementaryFiles[i].first == "Windows"
                         && wxCopyFile( supplementaryFiles[i].second, tempDir + "/" + supplementaryFiles[i].second, true ) == false )
                        diagnosticManager.AddError(ToString(_( "Impossible de copier ")+supplementaryFiles[i].second+_(" pour Windows dans le r�pertoire de compilation.\n" )));
                }
            }

            if ( linuxTarget )
            {
                const std::vector < std::pair<std::string, std::string> > & supplementaryFiles = extension->GetSupplementaryRuntimeFiles();
                for (unsigned int i = 0;i<supplementaryFiles.size();++i)
                {
                    if ( supplementaryFiles[i].first == "Linux"
                         && wxCopyFile( supplementaryFiles[i].second, tempDir + "/" + supplementaryFiles[i].second, true ) == false )
                        diagnosticManager.AddError(ToString(_( "Impossible de copier ")+supplementaryFiles[i].second+_(" pour Linux dans le r�pertoire de compilation.\n" )));
                }
            }

            if ( macTarget )
            {
                const std::vector < std::pair<std::string, std::string> > & supplementaryFiles = extension->GetSupplementaryRuntimeFiles();
                for (unsigned int i = 0;i<supplementaryFiles.size();++i)
                {
                    if ( supplementaryFiles[i].first == "Mac"
                         && wxCopyFile( supplementaryFiles[i].second, tempDir + "/" + supplementaryFiles[i].second, true ) == false )
                        diagnosticManager.AddError(ToString(_( "Impossible de copier ")+supplementaryFiles[i].second+_(" pour Mac OS dans le r�pertoire de compilation.\n" )));
                }
            }
        }
    }
    if ( game.useExternalSourceFiles )
    {
        if ( wxCopyFile( "dynext.dxgd", tempDir + "/" + "dynext.dxgd", true ) == false )
            diagnosticManager.AddError(ToString(_( "Impossible de copier les sources C++ compil�es ( dynext.dxgd ) dans le r�pertoire de compilation.\n" )));
    }

    //Copie des derniers fichiers
    if ( !compressIfPossible )
    {
        //Fichier pour windows
        if ( windowsTarget )
        {
            if ( wxCopyFile( "Runtime/PlayWin.exe", tempDir + "/" + winExecutableName, true ) == false )
                diagnosticManager.AddError(ToString(_( "Impossible de cr�er ")+"l'executable Windows"+_(" dans le r�pertoire de compilation.\n" )));

            if ( wxCopyFile( "Runtime/gdl.dll", tempDir + "/gdl.dll", true ) == false )
                diagnosticManager.AddError(ToString(_( "Impossible de cr�er ")+"l'executable gdl.dll"+_(" dans le r�pertoire de compilation.\n" )));

        }
        //Fichiers pour linux
        if ( linuxTarget )
        {
            if ( wxCopyFile( "Runtime/ExeLinux", tempDir + "/ExeLinux", true ) == false )
                diagnosticManager.AddError(ToString(_( "Impossible de cr�er ")+"l'executable Linux"+_(" dans le r�pertoire de compilation.\n" )));

            if ( wxCopyFile( "Runtime/PlayLinux", tempDir + "/" + linuxExecutableName, true ) == false )
                diagnosticManager.AddError(ToString(_( "Impossible de cr�er ")+"le script executable Linux"+_(" dans le r�pertoire de compilation.\n" )));

            if ( wxCopyFile( "Runtime/libgdl.so", tempDir + "/libgdl.so", true ) == false )
                diagnosticManager.AddError(ToString(_( "Impossible de cr�er ")+"libgdl.so"+_(" dans le r�pertoire de compilation.\n" )));
        }
        if ( macTarget )
        {
            if ( wxCopyFile( "MacRuntime/MacExe", tempDir + "/MacExe", true ) == false )
                diagnosticManager.AddError(ToString(_( "Impossible de cr�er ")+"l'executable Mac OS"+_(" dans le r�pertoire de compilation.\n" )));

            if ( wxCopyFile( "MacRuntime/libgdl.dylib", tempDir + "/libgdl.dylib", true ) == false )
                diagnosticManager.AddError(ToString(_( "Impossible de cr�er ")+"libgdl.dylib"+_(" dans le r�pertoire de compilation.\n" )));
        }

        //Copie du tout dans le r�pertoire final
        wxString file = wxFindFirstFile( tempDir + "/*" );
        while ( !file.empty() )
        {
            wxFileName fileName(file);
            if ( !wxCopyFile( file, outDir + "/" + fileName.GetFullName(), true ) )
                diagnosticManager.AddError(ToString(_( "Impossible de copier le fichier " + file + " depuis le r�pertoire de compilation vers le r�pertoire final.\n" )));

            file = wxFindNextFile();
        }
    }
    else
    {
        if ( windowsTarget )
        {
            if ( wxCopyFile( "Runtime/PlayWin.exe", tempDir + "/internalstart.exe", true ) == false )
                diagnosticManager.AddError(ToString(_( "Impossible de cr�er ")+"l'executable Windows"+_(" dans le r�pertoire de compilation.\n" )));

            if ( wxCopyFile( "Runtime/gdl.dll", tempDir + "/gdl.dll", true ) == false )
                diagnosticManager.AddError(ToString(_( "Impossible de cr�er ")+"l'executable gdl.dll"+_(" dans le r�pertoire de compilation.\n" )));

            //Use 7zip to create a single archive
            diagnosticManager.OnMessage( ToString( _("Exportation du jeu... ( Compression )") ) );
            wxArrayString arrStdOut, arrStdErr;
            wxExecute( _T( "7za.exe a  \""+ tempDir +"/archive.7z\" \"" + tempDir + "/*\"" ), arrStdOut, arrStdErr, wxEXEC_SYNC  );

            //Make the archive autoextractible
            std::ofstream outFile;
            outFile.open (std::string(outDir+"/"+winExecutableName).c_str(), std::ofstream::out | std::ofstream::binary);
            {
                std::ifstream file;
                char buffer[1];

                file.open ("7zS.sfx", std::ifstream::in | std::ifstream::binary);
                if (file.is_open())
                {
                    file.seekg (0, std::ios::beg);
                    while (file.read (buffer, 1))
                        outFile.write (buffer, 1);

                    file.close();
                }
                else
                    diagnosticManager.AddError( ToString(_("Unable to open 7zS.sfx")) );
            }
            {
                std::ifstream file;
                char buffer[1];

                file.open ("config.txt", std::ifstream::in | std::ifstream::binary);
                if (file.is_open())
                {
                    file.seekg (0, std::ios::beg);
                    while (file.read (buffer, 1))
                        outFile.write (buffer, 1);

                    file.close();
                }
                else
                    diagnosticManager.AddError( ToString(_("Unable to open config.txt")) );
            }
            {
                std::ifstream file;
                char buffer[1];

                file.open (std::string(tempDir +"/archive.7z").c_str(), std::ifstream::in | std::ifstream::binary);
                if (file.is_open())
                {
                    file.seekg (0, std::ios::beg);
                    while (file.read (buffer, 1))
                        outFile.write (buffer, 1);

                    file.close();
                }
                else
                    diagnosticManager.AddError( ToString(_("Unable to open "))+std::string(tempDir +"/archive.7z") );
            }

            outFile.close();
        }
    }

    //Prepare executables
    #if defined(WINDOWS)
    if ( windowsTarget )
        ExecutableIconChanger::ChangeWindowsExecutableIcon(string(outDir+"/"+winExecutableName), game.winExecutableIconFile);
    #endif

    diagnosticManager.OnMessage(ToString(_( "Compilation termin�e" )));
    diagnosticManager.OnPercentUpdate(100);

    diagnosticManager.OnCompilationSuccessed();
}


/**
 * Return a temporary directory
 */
std::string FullProjectCompiler::GetTempDir()
{
    std::string tempDir = forcedTempDir;
    if ( tempDir.empty() ) //If the user has not forced a directory
    {
        tempDir = wxFileName::GetTempDir();
        if ( !wxFileName::IsDirWritable(tempDir) )
            tempDir = wxGetCwd();

        if ( !wxFileName::IsDirWritable(tempDir) )
            tempDir = wxFileName::GetHomeDir();

        if ( !wxFileName::IsDirWritable(tempDir) )
            wxMessageBox(_("Game Develop n'a pas r�ussi � trouver un r�pertoire temporaire pour la compilation.\nSi la compilation �choue, allez dans les pr�f�rences et choisissez un r�pertoire temporaire o� vous avez les droits d'�criture."), _("La compilation risque d'�chouer."), wxICON_EXCLAMATION);
    }

    return tempDir + "/GDDeploymentTemporaries";
}

void FullProjectCompiler::ClearDirectory(std::string directory)
{
    if ( !wxDirExists( directory ) && !wxMkdir( directory ) )
            diagnosticManager.AddError(ToString(_( "Impossible de cr�er le r�pertoire : " ) + directory + "\n"));

    wxString file = wxFindFirstFile( directory + "/*" );
    while ( !file.empty() )
    {
        if ( !wxRemoveFile( file ) )
            diagnosticManager.AddError(ToString(_( "Impossible de supprimer le fichier " + file + " situ� dans le r�pertoire "+directory+".\n" )));

        file = wxFindNextFile();
    }
}


void FullProjectCompilerConsoleDiagnosticManager::OnCompilationFailed()
{
    cout << _("La compilation a �chou� avec ces erreurs :") << endl;
    cout << GetErrors();
}

void FullProjectCompilerConsoleDiagnosticManager::OnCompilationSuccessed()
{
    cout << _("La compilation a termin� sans erreurs.") << endl;
}

void FullProjectCompilerConsoleDiagnosticManager::OnMessage(std::string message, std::string message2)
{
    if ( message2.empty() )
        cout << message << endl;
    else
        cout << message << ": " << message2 << endl;
}

void FullProjectCompilerConsoleDiagnosticManager::OnPercentUpdate(float percents)
{
    cout << _("Avancement : ") << ToString(percents) << endl;
}


}
#endif
