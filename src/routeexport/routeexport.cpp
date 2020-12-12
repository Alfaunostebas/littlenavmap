/*****************************************************************************
* Copyright 2015-2020 Alexander Barthel alex@littlenavmap.org
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*****************************************************************************/

#include "routeexport/routeexport.h"

#include "routeexport/routeexportformat.h"
#include "routeexport/routeexportdialog.h"
#include "atools.h"
#include "common/aircrafttrack.h"
#include "common/constants.h"
#include "exception.h"
#include "fs/perf/aircraftperf.h"
#include "fs/pln/flightplanio.h"
#include "gui/dialog.h"
#include "gui/errorhandler.h"
#include "gui/mainwindow.h"
#include "io/fileroller.h"
#include "navapp.h"
#include "options/optiondata.h"
#include "route/routealtitude.h"
#include "route/routecontroller.h"
#include "routeexport/routeexportdata.h"
#include "routeexport/routemultiexportdialog.h"
#include "routestring/routestringwriter.h"
#include "ui_mainwindow.h"

#include <QBitArray>
#include <QDir>
#include <QProcessEnvironment>
#include <QXmlStreamReader>

using atools::fs::pln::FlightplanIO;

RouteExport::RouteExport(MainWindow *parent)
  : mainWindow(parent)
{
  exportFormatMap = new RouteExportFormatMap;
  flightplanIO = new FlightplanIO;
  dialog = new atools::gui::Dialog(mainWindow);
  exportAllDialog = new RouteMultiExportDialog(mainWindow, exportFormatMap);

  // Save now button in list
  connect(exportAllDialog, &RouteMultiExportDialog::saveNowButtonClicked, this, &RouteExport::exportType);
}

RouteExport::~RouteExport()
{
  delete exportAllDialog;
  delete dialog;
  delete flightplanIO;
  delete exportFormatMap;
}

void RouteExport::saveState()
{
  exportFormatMap->saveState();
}

void RouteExport::restoreState()
{
  exportFormatMap->restoreState();
  exportFormatMap->initCallbacks(this);
  exportAllDialog->restoreState();
  selected = exportFormatMap->hasSelected();
}

void RouteExport::postDatabaseLoad()
{
  exportFormatMap->updateDefaultPaths();
  selected = exportFormatMap->hasSelected();
}

void RouteExport::exportType(RouteExportFormat format)
{
  // Export only selected type with file dialog - triggered from multiexport dialog
  if(format.copyForManualSaveFileDialog(NavApp::getRouteFilepath()).callExport())
    mainWindow->setStatusMessage(tr("Exported flight plan."));
}

void RouteExport::formatExportedCallback(const RouteExportFormat& format, const QString& filename)
{
  exported.insert(format.getType(), filename);
}

void RouteExport::routeMultiExport()
{
  exported.clear();

  // Collect path errors first =======================
  // Check if selected paths exist
  exportFormatMap->updatePathErrors();
  QStringList errorFormats;
  for(const RouteExportFormat& fmt : exportFormatMap->getSelected())
  {
    if(fmt.isSelected() && !fmt.isPathValid())
      errorFormats.append(fmt.getComment());
  }

  if(!errorFormats.isEmpty())
  {
    // One or more paths are not valid - do not export
    QMessageBox::warning(mainWindow, QApplication::applicationName(),
                         tr("<p>The following export formats have an invalid path:</p>"
                              "<ul><li>%1</li></ul>"
                                "<p>Export stopped.</p>"
                                  "<p>Go to: Main menu -&gt; &quot;File&quot; -&gt; &quot;Multiexport Flight Plan Options&quot; or "
                                    "press Ctrl+Alt+Shift+M and correct the path highlighted in red.</p>").
                         arg(errorFormats.join("</li><li>")));
  }
  else
  {
    // Export - first check constraints for all formats - also updates AIRAC cycle
    if(routeValidate(exportFormatMap->getSelected(), true /* multi */))
    {
      // Export all button or menu item
      int numExported = 0;
      for(const RouteExportFormat& fmt : exportFormatMap->getSelected())
      {
        if(fmt.isSelected() && fmt.isPathValid())
          numExported += fmt.copyForMultiSave(NavApp::getRouteFilepath()).callExport();
      }
      if(numExported == 0)
        mainWindow->setStatusMessage(tr("No flight plan exported."));
      else
        mainWindow->setStatusMessage(tr("Exported %1 flight plans.").arg(numExported));
    }

    // Check if native LNMPLN was exported, update filename and change status of the file if
    if(exported.contains(rexp::LNMPLN))
      mainWindow->routeSaveLnmExported(exported.value(rexp::LNMPLN));
    exported.clear();
  }
}

void RouteExport::routeMultiExportOptions()
{
  int result = exportAllDialog->exec();

  if(result == QDialog::Accepted)
  {
    selected = exportFormatMap->hasSelected();

    // Dialog saves its own options
    saveState();
    emit optionsUpdated();
  }
}

QString RouteExport::exportFile(const RouteExportFormat& format, const QString& settingsPrefix,
                                const QString& path, const QString& filename, bool dontComfirmOverwrite)
{
  QString routeFile;

  if(format.isManual())
    // Called from menu actions ======================================
    routeFile = dialog->saveFileDialog(tr("Export for %1").arg(format.getComment()),
                                       tr("%1 Files %2;;All Files (*)").
                                       arg(format.getFormat().toUpper()).arg(format.getFilter()),
                                       format.getFormat(), settingsPrefix, path, filename, dontComfirmOverwrite);
  else
  {
    // Called from multiexport action or multiexport dialog ======================================
    if(format.isExportToFile())
    {
      // Append to file
      dontComfirmOverwrite = true;
    }

    // Build filename
    QString name;
    if(format.isExportToFile())
      name = format.getPath();
    else
      name = format.getPath() + QDir::separator() + filename;

    RouteMultiExportDialog::ExportOptions opts = exportAllDialog->getExportOptions();

    // Force open of file dialog since this was called from multiexport dialog
    if(format.isFileDialog())
      opts = RouteMultiExportDialog::FILEDIALOG;

    switch(opts)
    {
      // Show file dialog for all formats selected
      case RouteMultiExportDialog::FILEDIALOG:
        routeFile = dialog->saveFileDialog(
          tr("Export for %1").arg(format.getComment()),
          tr("%1 Files %2;;All Files (*)").
          arg(format.getFormat().toUpper()).arg(format.getFilter()),
          format.getFormat(),
          QString() /* settingsPrefix */,
          format.getPath(), filename, dontComfirmOverwrite);
        break;

      case RouteMultiExportDialog::RENAME_EXISTING:
        // Rotate for new files - otherwise keep it since appending is desired
        if(!format.isExportToFile())
          rotateFile(name);
        routeFile = name;
        break;

      case RouteMultiExportDialog::OVERWRITE:
        routeFile = name;
        break;
    }
  }
  return routeFile;
}

QString RouteExport::exportFileMulti(const RouteExportFormat& format, const QString& filename)
{
  return exportFile(format, QString() /* settingsPrefix */, QString() /* path */, filename, format.isExportToFile());
}

void RouteExport::rotateFile(const QString& filename)
{
  if(!QFile::exists(filename))
    return;
  else
  {
    QString separator = QFileInfo(filename).completeBaseName().contains(" ") ? " " : "_";
    atools::io::FileRoller(4, "${base}" + separator + "${num}.${ext}").rollFile(filename);
  }
}

bool RouteExport::routeExportPlnMan()
{
  return routeExportPln(exportFormatMap->getForManualSave(rexp::PLN, NavApp::getRouteFilepath()));
}

bool RouteExport::routeExportPlnMsfsMan()
{
  return routeExportPln(exportFormatMap->getForManualSave(rexp::PLNMSFS, NavApp::getRouteFilepath()));
}

bool RouteExport::routeExportPln(const RouteExportFormat& format)
{
  return routeExportInternalPln(format);
}

bool RouteExport::routeExportLnm(const RouteExportFormat& format)
{
  qDebug() << Q_FUNC_INFO;

  if(routeValidateMulti(format))
  {
    QString routeFile = exportFile(format, "Route/LnmPln",
                                   NavApp::getCurrentSimulatorFilesPath(), buildDefaultFilename(format),
                                   false /* dontComfirmOverwrite */);

    if(!routeFile.isEmpty())
    {
      using namespace std::placeholders;
      if(exportFlighplan(routeFile, rf::DEFAULT_OPTS_LNMPLN, std::bind(&FlightplanIO::saveLnm, flightplanIO, _1, _2)))
      {
        formatExportedCallback(format, routeFile);
        return true;
      }
    }
  }
  return false;
}

bool RouteExport::routeExportPlnMsfs(const RouteExportFormat& format)
{
  return routeExportInternalPln(format);
}

bool RouteExport::routeExportPlnAnnotatedMulti(const RouteExportFormat& format)
{
  return routeExportInternalPln(format);
}

bool RouteExport::routeExportInternalPln(const RouteExportFormat& format)
{
  qDebug() << Q_FUNC_INFO;

  if(routeValidateMulti(format))
  {
    bool msfs = format.getType() == rexp::PLNMSFS;

    QString routeFile = exportFile(format, "Route/Pln" + NavApp::getCurrentSimulatorShortName(),
                                   NavApp::getCurrentSimulatorFilesPath(), buildDefaultFilename(format, ".pln", msfs),
                                   false /* dontComfirmOverwrite */);

    if(!routeFile.isEmpty())
    {
      bool annotated = format.getType() == rexp::PLNANNOTATED;

      using namespace std::placeholders;
      auto func = msfs ?
                  std::bind(&FlightplanIO::savePlnMsfs, flightplanIO, _1, _2) :
                  (annotated ?
                   std::bind(&FlightplanIO::savePlnAnnotated, flightplanIO, _1, _2) :
                   std::bind(&FlightplanIO::savePln, flightplanIO, _1, _2));

      if(exportFlighplan(routeFile, msfs ? rf::DEFAULT_OPTS_MSFS : rf::DEFAULT_OPTS, func))
      {
        mainWindow->setStatusMessage(tr("Flight plan saved as %1PLN.").arg(annotated ? tr("annotated ") : QString()));
        formatExportedCallback(format, routeFile);
        return true;
      }
    }
  }
  return false;
}

bool RouteExport::routeExportFms3Multi(const RouteExportFormat& format)
{
  qDebug() << Q_FUNC_INFO;

  if(routeValidateMulti(format))
  {
    QString routeFile = exportFileMulti(format, buildDefaultFilenameShort("-", ".fms"));
    if(!routeFile.isEmpty())
    {
      using namespace std::placeholders;
      if(exportFlighplan(routeFile, rf::DEFAULT_OPTS_FMS3, std::bind(&FlightplanIO::saveFms3, flightplanIO, _1, _2)))
      {
        mainWindow->setStatusMessage(tr("Flight plan saved as FMS 3."));
        formatExportedCallback(format, routeFile);
        return true;
      }
    }
  }
  return false;
}

bool RouteExport::routeExportFms11Man()
{
  return routeExportFms11(exportFormatMap->getForManualSave(rexp::FMS11));
}

bool RouteExport::routeExportFms11(const RouteExportFormat& format)
{
  qDebug() << Q_FUNC_INFO;

  if(routeValidateMulti(format))
  {
    // Try to get X-Plane default output directory for flight plans
    QString xpBasePath = NavApp::getSimulatorBasePath(atools::fs::FsPaths::XPLANE11);
    if(xpBasePath.isEmpty())
      xpBasePath = atools::documentsDir();
    else
      xpBasePath = atools::buildPathNoCase({xpBasePath, "Output", "FMS plans"});

    QString routeFile = exportFile(format, "Route/Fms11", xpBasePath, buildDefaultFilenameShort("-", ".fms"),
                                   false /* dontComfirmOverwrite */);

    if(!routeFile.isEmpty())
    {
      using namespace std::placeholders;
      if(exportFlighplan(routeFile, rf::DEFAULT_OPTS_FMS11, std::bind(&FlightplanIO::saveFms11, flightplanIO, _1, _2)))
      {
        mainWindow->setStatusMessage(tr("Flight plan saved as FMS 11."));
        formatExportedCallback(format, routeFile);
        return true;
      }
    }
  }
  return false;
}

bool RouteExport::routeExportFlpCrjMulti(const RouteExportFormat& format)
{
  return routeExportInternalFlp(format, true /* CRJ */);
}

bool RouteExport::routeExportFlpMulti(const RouteExportFormat& format)
{
  return routeExportInternalFlp(format, false /* CRJ */);
}

bool RouteExport::routeExportInternalFlp(const RouteExportFormat& format, bool crj)
{
  qDebug() << Q_FUNC_INFO;
  if(routeValidateMulti(format))
  {
    // <Documents>/Aerosoft/Airbus/Flightplans.
    QString routeFile = exportFileMulti(format, buildDefaultFilenameShort(QString(), ".flp"));
    if(!routeFile.isEmpty())
    {
      using namespace std::placeholders;
      if(exportFlighplan(routeFile, rf::DEFAULT_OPTS,
                         crj ?
                         std::bind(&FlightplanIO::saveCrjFlp, flightplanIO, _1, _2) :
                         std::bind(&FlightplanIO::saveFlp, flightplanIO, _1, _2)))
      {
        formatExportedCallback(format, routeFile);
        return true;
      }
    }
  }
  return false;
}

bool RouteExport::routeExportFlightgearMan()
{
  return routeExportFlightgear(exportFormatMap->getForManualSave(rexp::FLIGHTGEAR, NavApp::getRouteFilepath()));
}

bool RouteExport::routeExportFlightgear(const RouteExportFormat& format)
{
  qDebug() << Q_FUNC_INFO;
  if(routeValidateMulti(format))
  {
    QString routeFile = exportFile(format, "Route/FlightGear", atools::documentsDir(),
                                   buildDefaultFilename(format, ".fgfp"),
                                   false /* dontComfirmOverwrite */);

    if(!routeFile.isEmpty())
    {
      using namespace std::placeholders;
      if(exportFlighplan(routeFile, rf::DEFAULT_OPTS, std::bind(&FlightplanIO::saveFlightGear, flightplanIO, _1, _2)))
      {
        formatExportedCallback(format, routeFile);
        return true;
      }
    }
  }
  return false;
}

/* Called from menu or toolbar by action */
bool RouteExport::routeExportRxpGnsMulti(const RouteExportFormat& format)
{
  qDebug() << Q_FUNC_INFO;

  // Save flight plan as FPL file usable by the GNS 530W/430W V2 - XML format
  if(routeValidateMulti(format))
  {
    QString path;

#ifdef Q_OS_WIN32
    QString gnsPath(QProcessEnvironment::systemEnvironment().value("GNSAPPDATA"));
    path = gnsPath.isEmpty() ? "C:\\ProgramData\\Garmin\\GNS Trainer Data\\GNS\\FPL" : gnsPath + "\\FPL";
#elif DEBUG_INFORMATION
    path = atools::buildPath({atools::documentsDir(), "Garmin", "GNS Trainer Data", "GNS", "FPL"});
#else
    path = atools::documentsDir();
#endif

    bool mkdir = QDir(path).mkpath(path);
    qInfo() << "mkdir" << path << "result" << mkdir;

    QString routeFile = exportFileMulti(format, buildDefaultFilenameShort("", ".fpl"));
    if(!routeFile.isEmpty())
    {
      if(exportFlighplanAsRxpGns(routeFile, format.getFlags().testFlag(rexp::GARMIN_AS_WAYPOINTS)))
      {
        formatExportedCallback(format, routeFile);
        return true;
      }
    }
  }
  return false;
}

/* Called from menu or toolbar by action */
bool RouteExport::routeExportRxpGtnMulti(const RouteExportFormat& format)
{
  qDebug() << Q_FUNC_INFO;

  // Save flight plan as GFP file usable by the Reality XP GTN 750/650 Touch
  if(routeValidateMulti(format))
  {
    QString path;

    // Location depends on trainer version - this is all above 6.41
#ifdef Q_OS_WIN32
    QString gtnPath(QProcessEnvironment::systemEnvironment().value("GTNSIMDATA"));
    path = gtnPath.isEmpty() ? "C:\\ProgramData\\Garmin\\Trainers\\Databases\\FPLN" : gtnPath + "\\Databases\\FPLN";
#elif DEBUG_INFORMATION
    path = atools::buildPath({atools::documentsDir(), "Garmin", "Trainers", "GTN", "FPLN"});
#else
    path = atools::documentsDir();
#endif

    bool mkdir = QDir(path).mkpath(path);
    qInfo() << "mkdir" << path << "result" << mkdir;

    QString routeFile = exportFileMulti(format, buildDefaultFilenameShort("_", ".gfp"));
    if(!routeFile.isEmpty())
    {
      if(exportFlighplanAsRxpGtn(routeFile, format.getFlags().testFlag(rexp::GARMIN_AS_WAYPOINTS)))
      {
        formatExportedCallback(format, routeFile);
        return true;
      }
    }
  }
  return false;
}

/* Called from menu or toolbar by action */
bool RouteExport::routeExportGfpMulti(const RouteExportFormat& format)
{
  qDebug() << Q_FUNC_INFO;

  // <FSX/P3D>/F1GTN/FPL.
  if(routeValidateMulti(format))
  {
    QString routeFile = exportFileMulti(format, buildDefaultFilenameShort("-", ".gfp"));
    if(!routeFile.isEmpty())
    {
      if(exportFlighplanAsGfp(routeFile, format.getFlags().testFlag(rexp::GARMIN_AS_WAYPOINTS)))
      {
        formatExportedCallback(format, routeFile);
        return true;
      }
    }
  }
  return false;
}

/* Called from menu or toolbar by action */
bool RouteExport::routeExportTxtMulti(const RouteExportFormat& format)
{
  qDebug() << Q_FUNC_INFO;

  if(routeValidateMulti(format))
  {
    QString routeFile = exportFileMulti(format, buildDefaultFilenameShort(QString(), ".txt"));
    if(!routeFile.isEmpty())
    {
      if(exportFlighplanAsTxt(routeFile))
      {
        formatExportedCallback(format, routeFile);
        return true;
      }
    }
  }
  return false;
}

bool RouteExport::routeExportRteMulti(const RouteExportFormat& format)
{
  qDebug() << Q_FUNC_INFO;

  if(routeValidateMulti(format))
  {
    QString routeFile = exportFileMulti(format, buildDefaultFilenameShort(QString(), ".rte"));
    if(!routeFile.isEmpty())
    {
      using namespace std::placeholders;
      if(exportFlighplan(routeFile, rf::DEFAULT_OPTS, std::bind(&FlightplanIO::saveRte, flightplanIO, _1, _2)))
      {
        formatExportedCallback(format, routeFile);
        return true;
      }
    }
  }
  return false;
}

bool RouteExport::routeExportFprMulti(const RouteExportFormat& format)
{
  qDebug() << Q_FUNC_INFO;

  // <FSX/P3D>/SimObjects/Airplanes/mjc8q400/nav/routes.

  if(routeValidateMulti(format))
  {
    QString routeFile = exportFileMulti(format, buildDefaultFilenameShort(QString(), ".fpr"));
    if(!routeFile.isEmpty())
    {
      using namespace std::placeholders;
      if(exportFlighplan(routeFile, rf::DEFAULT_OPTS, std::bind(&FlightplanIO::saveFpr, flightplanIO, _1, _2)))
      {
        formatExportedCallback(format, routeFile);
        return true;
      }
    }
  }
  return false;
}

bool RouteExport::routeExportFplMulti(const RouteExportFormat& format)
{
  qDebug() << Q_FUNC_INFO;

  // \X-Plane 11\Aircraft\X-Aviation\IXEG 737 Classic\coroutes
  if(routeValidateMulti(format))
  {
    QString routeFile = exportFileMulti(format, buildDefaultFilenameShort(QString(), ".fpl"));
    if(!routeFile.isEmpty())
    {
      // Same format as txt
      if(exportFlighplanAsTxt(routeFile))
      {
        formatExportedCallback(format, routeFile);
        return true;
      }
    }
  }
  return false;
}

bool RouteExport::routeExportCorteInMulti(const RouteExportFormat& format)
{
  qDebug() << Q_FUNC_INFO;

  if(routeValidateMulti(format))
  {
    QString routeFile = exportFileMulti(format, "corte.in");
    if(!routeFile.isEmpty())
    {
      if(exportFlighplanAsCorteIn(routeFile))
      {
        formatExportedCallback(format, routeFile);
        return true;
      }
    }
  }
  return false;
}

bool RouteExport::routeExportFltplanMulti(const RouteExportFormat& format)
{
  qDebug() << Q_FUNC_INFO;
  // Default directory for the iFly stored Flight plans is Prepar3D/iFly/737NG/navdata/FLTPLAN
  // <FSX/P3D>/iFly/737NG/navdata/FLTPLAN.
  // YSSYYMML.FLTPLAN

  if(routeValidateMulti(format))
  {
    QString routeFile = exportFileMulti(format, buildDefaultFilenameShort(QString(), ".fltplan"));
    if(!routeFile.isEmpty())
    {
      using namespace std::placeholders;
      if(exportFlighplan(routeFile, rf::DEFAULT_OPTS, std::bind(&FlightplanIO::saveFltplan, flightplanIO, _1, _2)))
      {
        formatExportedCallback(format, routeFile);
        return true;
      }
    }
  }
  return false;
}

bool RouteExport::routeExportXFmcMulti(const RouteExportFormat& format)
{
  qDebug() << Q_FUNC_INFO;
  // The plans have an extension of *.FPL and are saved in for following Folder : -
  // Path to Root X-Plane 11 Folder\Resources\plugins\XFMC\FlightPlans
  // LFLLEHAM.FPL
  // Same as TXT but FPL

  if(routeValidateMulti(format))
  {
    QString routeFile = exportFileMulti(format, buildDefaultFilenameShort(QString(), ".fpl"));
    if(!routeFile.isEmpty())
    {
      if(exportFlighplanAsTxt(routeFile))
      {
        formatExportedCallback(format, routeFile);
        return true;
      }
    }
  }
  return false;
}

bool RouteExport::routeExportUFmcMulti(const RouteExportFormat& format)
{
  qDebug() << Q_FUNC_INFO;
  // EDDHLIRF.ufmc
  if(routeValidateMulti(format))
  {
    QString routeFile = exportFileMulti(format, buildDefaultFilenameShort(QString(), ".ufmc"));
    if(!routeFile.isEmpty())
    {
      if(exportFlighplanAsUFmc(routeFile))
      {
        formatExportedCallback(format, routeFile);
        return true;
      }
    }
  }
  return false;
}

bool RouteExport::routeExportProSimMulti(const RouteExportFormat& format)
{
  // companyroutes.xml
  qDebug() << Q_FUNC_INFO;

  if(routeValidateMulti(format))
  {
    QString routeFile = exportFileMulti(format, "companyroutes.xml");
    if(!routeFile.isEmpty())
    {
      if(exportFlighplanAsProSim(routeFile))
      {
        formatExportedCallback(format, routeFile);
        return true;
      }
    }
  }
  return false;
}

bool RouteExport::routeExportBbsMulti(const RouteExportFormat& format)
{
  qDebug() << Q_FUNC_INFO;
  // P3D Root Folder \BlackBox Simulation\Airbus A330
  // <FSX/P3D>/Blackbox Simulation/Company Routes.
  // Uses FS9 PLN format.   EDDHLIRF.pln

  if(routeValidateMulti(format))
  {
    QString routeFile = exportFileMulti(format, buildDefaultFilenameShort(QString(), ".pln"));
    if(!routeFile.isEmpty())
    {
      using namespace std::placeholders;
      if(exportFlighplan(routeFile, rf::DEFAULT_OPTS, std::bind(&FlightplanIO::saveBbsPln, flightplanIO, _1, _2)))
      {
        formatExportedCallback(format, routeFile);
        return true;
      }
    }
  }
  return false;
}

bool RouteExport::routeExportFeelthereFplMulti(const RouteExportFormat& format)
{
  qDebug() << Q_FUNC_INFO;
  if(routeValidateMulti(format))
  {
    QString routeFile = exportFileMulti(format, buildDefaultFilenameShort("_", ".fpl"));
    if(!routeFile.isEmpty())
    {
      int groundSpeed = atools::roundToInt(NavApp::getAltitudeLegs().getAverageGroundSpeed());
      if(groundSpeed < 5)
        groundSpeed = atools::roundToInt(NavApp::getAircraftPerformance().getCruiseSpeed());

      using namespace std::placeholders;
      if(exportFlighplan(routeFile, rf::DEFAULT_OPTS,
                         std::bind(&FlightplanIO::saveFeelthereFpl, flightplanIO, _1, _2, groundSpeed)))
      {
        formatExportedCallback(format, routeFile);
        return true;
      }
    }
  }
  return false;
}

bool RouteExport::routeExportLeveldRteMulti(const RouteExportFormat& format)
{
  qDebug() << Q_FUNC_INFO;
  if(routeValidateMulti(format))
  {
    QString routeFile = exportFileMulti(format, buildDefaultFilenameShort("_", ".rte"));
    if(!routeFile.isEmpty())
    {
      using namespace std::placeholders;
      if(exportFlighplan(routeFile, rf::DEFAULT_OPTS, std::bind(&FlightplanIO::saveLeveldRte, flightplanIO, _1, _2)))
      {
        formatExportedCallback(format, routeFile);
        return true;
      }
    }
  }
  return false;
}

bool RouteExport::routeExportEfbrMulti(const RouteExportFormat& format)
{
  qDebug() << Q_FUNC_INFO;
  if(routeValidateMulti(format))
  {
    QString routeFile = exportFileMulti(format, buildDefaultFilenameShort("_", ".efbr"));
    if(!routeFile.isEmpty())
    {
      QString route = RouteStringWriter().createStringForRoute(NavApp::getRouteConst(), 0.f, rs::NONE);
      QString cycle = NavApp::getDatabaseAiracCycleNav();
      using namespace std::placeholders;
      if(exportFlighplan(routeFile, rf::DEFAULT_OPTS,
                         std::bind(&FlightplanIO::saveEfbr, flightplanIO, _1, _2, route, cycle, QString(), QString())))
      {
        formatExportedCallback(format, routeFile);
        return true;
      }
    }
  }
  return false;
}

bool RouteExport::routeExportQwRteMulti(const RouteExportFormat& format)
{
  qDebug() << Q_FUNC_INFO;
  if(routeValidateMulti(format))
  {
    QString routeFile = exportFileMulti(format, buildDefaultFilenameShort(QString(), ".rte"));
    if(!routeFile.isEmpty())
    {
      using namespace std::placeholders;
      if(exportFlighplan(routeFile, rf::DEFAULT_OPTS, std::bind(&FlightplanIO::saveQwRte, flightplanIO, _1, _2)))
      {
        formatExportedCallback(format, routeFile);
        return true;
      }
    }
  }
  return false;
}

bool RouteExport::routeExportMdrMulti(const RouteExportFormat& format)
{
  qDebug() << Q_FUNC_INFO;
  if(routeValidateMulti(format))
  {
    QString routeFile = exportFileMulti(format, buildDefaultFilenameShort(QString(), ".mdr"));
    if(!routeFile.isEmpty())
    {
      using namespace std::placeholders;
      if(exportFlighplan(routeFile, rf::DEFAULT_OPTS, std::bind(&FlightplanIO::saveMdr, flightplanIO, _1, _2)))
      {
        formatExportedCallback(format, routeFile);
        return true;
      }
    }
  }
  return false;
}

bool RouteExport::routeExportTfdiMulti(const RouteExportFormat& format)
{
  // {Simulator}\SimObjects\Airplanes\TFDi_Design_717\Documents\Company Routes/

  qDebug() << Q_FUNC_INFO;
  if(routeValidateMulti(format))
  {
    QString routeFile = exportFileMulti(format, buildDefaultFilenameShort(QString(), ".xml"));
    if(!routeFile.isEmpty())
    {
      try
      {
        Route route = buildAdjustedRoute(rf::DEFAULT_OPTS);
        flightplanIO->saveTfdi(route.getFlightplan(), routeFile, route.getJetAirwayFlags());
      }
      catch(atools::Exception& e)
      {
        atools::gui::ErrorHandler(mainWindow).handleException(e);
        return false;
      }
      catch(...)
      {
        atools::gui::ErrorHandler(mainWindow).handleUnknownException();
        return false;
      }

      formatExportedCallback(format, routeFile);
      return true;
    }
  }
  return false;
}

bool RouteExport::routeExportVfpMan()
{
  return routeExportVfp(exportFormatMap->getForManualSave(rexp::VFP));
}

bool RouteExport::routeExportVfp(const RouteExportFormat& format)
{
  qDebug() << Q_FUNC_INFO;
  if(routeValidateMulti(format))
  {
    RouteExportData exportData = createRouteExportData(re::VFP);
    if(routeExportDialog(exportData, re::VFP))
    {
      QString routeFile = exportFileMulti(format, buildDefaultFilenameShort(QString(), ".vfp"));

      if(!routeFile.isEmpty())
      {
        if(exportFlighplanAsVfp(exportData, routeFile))
        {
          formatExportedCallback(format, routeFile);
          return true;
        }
      }
    }
  }
  return false;
}

bool RouteExport::routeExportXIvapMan()
{
  return routeExportXIvap(exportFormatMap->getForManualSave(rexp::XIVAP));
}

bool RouteExport::routeExportXIvap(const RouteExportFormat& format)
{
  return routeExportIvapInternal(re::XIVAP, format);
}

bool RouteExport::routeExportIvapMan()
{
  return routeExportIvap(exportFormatMap->getForManualSave(rexp::IVAP));
}

bool RouteExport::routeExportIvap(const RouteExportFormat& format)
{
  return routeExportIvapInternal(re::IVAP, format);
}

bool RouteExport::routeExportIvapInternal(re::RouteExportType type, const RouteExportFormat& format)
{
  qDebug() << Q_FUNC_INFO;

  if(routeValidateMulti(format))
  {
    RouteExportData exportData = createRouteExportData(type);
    if(routeExportDialog(exportData, type))
    {
      QString typeStr = RouteExportDialog::getRouteTypeAsDisplayString(type);
      QString routeFile = exportFileMulti(format, buildDefaultFilenameShort(QString(), ".fpl"));
      if(!routeFile.isEmpty())
      {
        if(exportFlighplanAsIvap(exportData, routeFile, type))
        {
          formatExportedCallback(format, routeFile);
          return true;
        }
      }
    }
  }
  return false;
}

RouteExportData RouteExport::createRouteExportData(re::RouteExportType routeExportType)
{
  RouteExportData exportData;

  const Route& route = NavApp::getRouteConst();
  exportData.setDeparture(route.getFlightplan().getDepartureIdent());
  exportData.setDestination(route.getFlightplan().getDestinationIdent());
  exportData.setDepartureTime(QDateTime::currentDateTimeUtc().time());
  exportData.setDepartureTimeActual(QDateTime::currentDateTimeUtc().time());
  exportData.setCruiseAltitude(atools::roundToInt(route.getCruisingAltitudeFeet()));

  QStringList alternates = route.getAlternateIdents();
  if(alternates.size() > 0)
    exportData.setAlternate(alternates.at(0));
  if(alternates.size() > 1)
    exportData.setAlternate2(alternates.at(1));

  atools::fs::pln::FlightplanType flightplanType = route.getFlightplan().getFlightplanType();
  switch(routeExportType)
  {
    case re::UNKNOWN:
      break;

    case re::VFP:
      // <?xml version="1.0" encoding="utf-8"?>
      // <FlightPlan xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xmlns:xsd="http://www.w3.org/2001/XMLSchema"
      // FlightType="IFR"
      exportData.setFlightRules(flightplanType == atools::fs::pln::IFR ? "IFR" : "VFR");
      exportData.setRoute(RouteStringWriter().createStringForRoute(route, 0.f, rs::SID_STAR));
      exportData.setVoiceType("Full");
      break;

    case re::IVAP:
    case re::XIVAP:
      exportData.setFlightRules(flightplanType == atools::fs::pln::IFR ? "I" : "V");
      exportData.setRoute(RouteStringWriter().createStringForRoute(route, 0.f, rs::SID_STAR | rs::DCT));
      break;

  }

  const RouteAltitude& routeAlt = NavApp::getAltitudeLegs();
  exportData.setAircraftType(NavApp::getAircraftPerformance().getAircraftType());
  exportData.setSpeed(atools::roundToInt(NavApp::getRouteCruiseSpeedKts()));
  exportData.setEnrouteMinutes(atools::roundToInt(routeAlt.getTravelTimeHours() * 60.f));
  exportData.setEnduranceMinutes(atools::roundToInt(routeAlt.getTravelTimeHours() * 60.f) + 60);

  return exportData;
}

bool RouteExport::routeExportDialog(RouteExportData& exportData, re::RouteExportType flightplanType)
{
  RouteExportDialog exportDialog(mainWindow, flightplanType);
  exportDialog.setExportData(exportData);
  if(exportDialog.exec() == QDialog::Accepted)
  {
    exportData = exportDialog.getExportData();
    return true;
  }
  return false;
}

bool RouteExport::routeExportGpxMan()
{
  return routeExportGpx(exportFormatMap->getForManualSave(rexp::GPX, NavApp::getRouteFilepath()));
}

bool RouteExport::routeExportGpx(const RouteExportFormat& format)
{
  qDebug() << Q_FUNC_INFO;

  QString routeFile = exportFile(format, "Route/Gpx", atools::documentsDir(),
                                 buildDefaultFilename(format, ".gpx"),
                                 false /* dontComfirmOverwrite */);

  if(!routeFile.isEmpty())
  {
    if(exportFlightplanAsGpx(routeFile))
    {
      if(NavApp::getAircraftTrack().isEmpty())
        mainWindow->setStatusMessage(tr("Flight plan saved as GPX."));
      else
        mainWindow->setStatusMessage(tr("Flight plan and track saved as GPX."));
      return true;
    }
  }
  return false;
}

bool RouteExport::routeExportHtmlMan()
{
  return routeExportHtml(exportFormatMap->getForManualSave(rexp::HTML, NavApp::getRouteFilepath()));
}

bool RouteExport::routeExportHtml(const RouteExportFormat& format)
{
  qDebug() << Q_FUNC_INFO;

  QString routeFile = exportFile(format, "Route/Html", atools::documentsDir(),
                                 buildDefaultFilename(format, ".html"),
                                 false /* dontComfirmOverwrite */);

  if(!routeFile.isEmpty())
  {
    QFile file(routeFile);
    if(file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
      QTextStream stream(&file);
      stream.setCodec("UTF-8");
      stream << NavApp::getRouteController()->getFlightplanTableAsHtmlDoc(24 /* iconSizePixel */);
      mainWindow->setStatusMessage(tr("Flight plan saved as HTML."));
      return true;
    }
    else
      atools::gui::ErrorHandler(mainWindow).handleIOError(file);
  }
  return false;
}

bool RouteExport::routeValidateMulti(const RouteExportFormat& format)
{
  if(format.isMulti())
    // Constraints are validated before running the export loop
    return true;
  else
    // Manual export
    return routeValidate({format});
}

/* Check if route has valid departure  and destination and departure parking.
 *  @return true if route can be saved anyway */
bool RouteExport::routeValidate(const QVector<RouteExportFormat>& formats, bool multi)
{
  NavApp::updateRouteCycleMetadata();

  bool validateParking = false, validateDepartAndDest = false, validateCycle = false;
  for(const RouteExportFormat& fmt : formats)
  {
    validateParking |= fmt.getFlags().testFlag(rexp::PARKING);
    validateDepartAndDest |= fmt.getFlags().testFlag(rexp::AIRPORTS);
    validateCycle |= fmt.getFlags().testFlag(rexp::CYCLE);
  }

  NavApp::deleteSplashScreen();

  // Default is to contue and save all formats
  bool save = true;
  const Route& route = NavApp::getRouteConst();

  QString doNotShowAgainText;
  if(multi)
    doNotShowAgainText = tr("Do not &show this dialog again and export all files in the future.");
  else
    doNotShowAgainText = tr("Do not &show this dialog again and save Flight Plan in the future.");
  QString reallyContinue = tr("\n\nReally continue?");

  // Check for valid airports for departure and destination ================================
  if(validateDepartAndDest && (!route.hasValidDeparture() || !route.hasValidDestination()))
  {
    const static atools::gui::DialogButtonList BUTTONS({
      {QString(), QMessageBox::Cancel},
      {tr("Select Start &Position"), QMessageBox::Yes},
      {QString(), QMessageBox::Save}
    });
    QString message;

    if(multi)
      message = tr("Flight Plan must have a valid airport as start and destination and "
                   "might not be usable for the selected export formats.");
    else
      message = tr("Flight Plan must have a valid airport as start and destination and "
                   "might not be usable by the simulator.");

    message += reallyContinue;

    int result = dialog->showQuestionMsgBox(
      multi ? lnm::ACTIONS_SHOWROUTE_WARNING_MULTI : lnm::ACTIONS_SHOWROUTE_WARNING,
      message, doNotShowAgainText, QMessageBox::Yes | QMessageBox::No, QMessageBox::No, QMessageBox::Yes);

    if(result == QMessageBox::Cancel)
      save = false;
  }

  // Check AIRAC cycle is not available but user saves for X-Plane  ================================
  if(save && validateCycle && NavApp::getDatabaseAiracCycleNav().isEmpty())
  {
    QString message;
    if(multi)
      message = tr("One or more of the selected export formats require a valid AIRAC cycle.\n\n");
    else
      message = tr("The export format requires a valid AIRAC cycle.\n\n");

    message += tr("The selected scenery database does not contain AIRAC cycle information.\n"
                  "This can happen if you save a flight plan based on FSX, Prepar3D or MSFS scenery.");

    message += reallyContinue;

    int result = dialog->showQuestionMsgBox(
      lnm::ACTIONS_SHOWROUTE_NO_CYCLE_WARNING,
      message, doNotShowAgainText, QMessageBox::Yes | QMessageBox::No, QMessageBox::No, QMessageBox::Yes);

    if(result == QMessageBox::No)
      save = false;
  }

  // Check if user forgot to select parking for an airport which has it ================================
  if(save && validateParking && route.hasValidDeparture() && !route.hasValidParking())
  {
    // Airport has parking but no one is selected
    const static atools::gui::DialogButtonList BUTTONS({
      {QString(), QMessageBox::Cancel},
      {tr("Select Start &Position"), QMessageBox::Yes},
      {tr("Show &Departure on Map"), QMessageBox::YesToAll},
      {QString(), QMessageBox::Save}
    });

    QString message;
    if(multi)
      message = tr("One or more of the selected export formats support "
                   "setting a parking spot as a start position.\n\n");

    message += tr("The departure airport has parking spots but no parking was selected for this Flight Plan.");
    int result = dialog->showQuestionMsgBox(
      lnm::ACTIONS_SHOWROUTE_PARKING_WARNING,
      message,
      tr("Do not &show this dialog again and save Flight Plan in the future."),
      BUTTONS, QMessageBox::Yes, QMessageBox::Save);

    if(result == QMessageBox::Yes)
      // saving depends if user selects parking or cancels out of the dialog
      save = emit selectDepartureParking();
    else if(result == QMessageBox::YesToAll)
    {
      // Zoom to airport and cancel out
      emit showRect(route.getDepartureAirportLeg().getAirport().bounding, false);
      save = false;
    }
    else if(result == QMessageBox::Cancel)
      save = false;
  }

  return save;
}

QString RouteExport::buildDefaultFilename(const QString& suffix, bool normalize)
{
  return buildDefaultFilename(NavApp::getRouteConst().getFlightplan(), suffix, normalize);
}

QString RouteExport::buildDefaultFilename(const RouteExportFormat&, const QString& suffix, bool normalize)
{
  return buildDefaultFilename(NavApp::getRouteConst().getFlightplan(), suffix, normalize);
}

QString RouteExport::buildDefaultFilename(const atools::fs::pln::Flightplan& plan, const QString& suffix,
                                          bool normalize)
{
  QString name = plan.getFilenamePattern(OptionData::instance().getFlightplanPattern(), suffix);
  return normalize ? atools::normalizeStr(name) : name;
}

QString RouteExport::buildDefaultFilenameShort(const QString& sep, const QString& suffix)
{
  return NavApp::getRouteConst().getFlightplan().getFilenamePattern(
    atools::fs::pln::pattern::DEPARTIDENT + sep + atools::fs::pln::pattern::DESTIDENT, suffix);
}

bool RouteExport::exportFlighplanAsGfp(const QString& filename, bool saveAsUserWaypoints)
{
  qDebug() << Q_FUNC_INFO << filename;
  QString gfp = RouteStringWriter().createGfpStringForRoute(
    buildAdjustedRoute(rf::DEFAULT_OPTS), false /* procedures */, saveAsUserWaypoints);

  QFile file(filename);
  if(file.open(QFile::WriteOnly | QIODevice::Text))
  {
    QByteArray utf8 = gfp.toUtf8();
    file.write(utf8.data(), utf8.size());
    file.close();
    return true;
  }
  else
  {
    atools::gui::ErrorHandler(mainWindow).handleIOError(file, tr("While saving GFP file:"));
    return false;
  }
}

bool RouteExport::exportFlighplanAsTxt(const QString& filename)
{
  qDebug() << Q_FUNC_INFO << filename;
  QString txt = RouteStringWriter().createStringForRoute(
    buildAdjustedRoute(rf::DEFAULT_OPTS), 0.f, rs::DCT | rs::START_AND_DEST | rs::SID_STAR_GENERIC);

  QFile file(filename);
  if(file.open(QFile::WriteOnly | QIODevice::Text))
  {
    QByteArray utf8 = txt.toUtf8();
    file.write(utf8.data(), utf8.size());
    file.close();
    return true;
  }
  else
  {
    atools::gui::ErrorHandler(mainWindow).handleIOError(file, tr("While saving TXT or FPL file:"));
    return false;
  }
}

bool RouteExport::exportFlighplanAsUFmc(const QString& filename)
{
  qDebug() << Q_FUNC_INFO << filename;
  QStringList list = RouteStringWriter().createStringForRouteList(
    buildAdjustedRoute(rf::DEFAULT_OPTS), 0.f, rs::DCT | rs::START_AND_DEST);

  // Remove last DCT
  if(list.size() - 2 >= 0 && list.at(list.size() - 2) == "DCT")
    list.removeAt(list.size() - 2);

  // Replace DCT with DIRECT
  list.replaceInStrings(QRegularExpression("^DCT$"), "DIRECT");
  // KJFK
  // CYYZ
  // DIRECT
  // GAYEL
  // Q818
  // WOZEE
  // 99
  QFile file(filename);
  if(file.open(QFile::WriteOnly | QIODevice::Text))
  {
    QTextStream stream(&file);
    // Save start and destination
    stream << list.first() << endl << list.last() << endl;

    // Waypoints and airways
    for(int i = 1; i < list.size() - 1; i++)
      stream << list.at(i) << endl;

    // File end
    stream << "99" << endl;

    file.close();
    return true;
  }
  else
  {
    atools::gui::ErrorHandler(mainWindow).handleIOError(file, tr("While saving UFMC file:"));
    return false;
  }
}

bool RouteExport::exportFlighplanAsRxpGns(const QString& filename, bool saveAsUserWaypoints)
{
  qDebug() << Q_FUNC_INFO << filename;

  try
  {
    // Regions are required for the export
    NavApp::getRoute().updateAirportRegions();
    FlightplanIO().saveGarminFpl(buildAdjustedRoute(rf::DEFAULT_OPTS).getFlightplan(), filename, saveAsUserWaypoints);
  }
  catch(atools::Exception& e)
  {
    atools::gui::ErrorHandler(mainWindow).handleException(e);
    return false;
  }
  catch(...)
  {
    atools::gui::ErrorHandler(mainWindow).handleUnknownException();
    return false;
  }
  return true;
}

bool RouteExport::exportFlighplanAsRxpGtn(const QString& filename, bool saveAsUserWaypoints)
{
  qDebug() << Q_FUNC_INFO << filename;
  QString gfp = RouteStringWriter().createGfpStringForRoute(
    buildAdjustedRoute(rf::DEFAULT_OPTS), true /* procedures */, saveAsUserWaypoints);

  QFile file(filename);
  if(file.open(QFile::WriteOnly | QIODevice::Text))
  {
    QByteArray utf8 = gfp.toUtf8();
    file.write(utf8.data(), utf8.size());
    file.close();
    return true;
  }
  else
  {
    atools::gui::ErrorHandler(mainWindow).handleIOError(file, tr("While saving GFP file:"));
    return false;
  }
}

bool RouteExport::exportFlighplanAsVfp(const RouteExportData& exportData, const QString& filename)
{
  QFile file(filename);
  if(file.open(QFile::WriteOnly | QIODevice::Text))
  {
    // <?xml version="1.0" encoding="utf-8"?>
    // <FlightPlan xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xmlns:xsd="http://www.w3.org/2001/XMLSchema"
    // FlightType="IFR"
    // Equipment="/L"
    // CruiseAltitude="23000"
    // CruiseSpeed="275"
    // DepartureAirport="KBZN"
    // DestinationAirport="KJAC"
    // AlternateAirport="KSLC"
    // Route="DCT 4529N11116W 4527N11114W 4524N11112W 4522N11110W DCT 4520N11110W 4519N11111W/N0276F220 4517N11113W 4516N11115W 4514N11115W/N0275F230 4509N11114W 4505N11113W 4504N11111W 4502N11107W 4500N11105W 4458N11104W 4452N11103W 4450N11105W/N0276F220 4449N11108W 4449N11111W 4449N11113W 4450N11116W 4451N11119W 4450N11119W/N0275F230 4448N11117W 4446N11116W DCT KWYS DCT 4440N11104W 4440N11059W 4439N11055W 4439N11052W 4435N11050W 4430N11050W 4428N11050W 4426N11044W 4427N11041W 4425N11035W 4429N11032W 4428N11031W 4429N11027W 4429N11025W 4432N11024W 4432N11022W 4432N11018W 4428N11017W 4424N11017W 4415N11027W/N0276F220 DCT 4409N11040W 4403N11043W DCT 4352N11039W DCT"
    // Remarks="PBN/D2 DOF/181102 REG/N012SB PER/B RMK/TCAS SIMBRIEF"
    // IsHeavy="false"
    // EquipmentPrefix=""
    // EquipmentSuffix="L"
    // DepartureTime="2035"
    // DepartureTimeAct="0"
    // EnrouteHours="0"
    // EnrouteMinutes="53"
    // FuelHours="2"
    // FuelMinutes="44"
    // VoiceType="Full" />
    QXmlStreamWriter writer(&file);
    writer.setCodec("UTF-8");
    writer.setAutoFormatting(true);
    writer.setAutoFormattingIndent(2);

    writer.writeStartDocument("1.0");
    writer.writeStartElement("FlightPlan");

    writer.writeAttribute("xmlns:xsi", "http://www.w3.org/2001/XMLSchema-instance");
    writer.writeAttribute("xmlns:xsd", "http://www.w3.org/2001/XMLSchema");

    writer.writeAttribute("FlightType", exportData.getFlightRules());
    writer.writeAttribute("Equipment", exportData.getEquipment());
    writer.writeAttribute("CruiseAltitude", QString::number(exportData.getCruiseAltitude()));
    writer.writeAttribute("CruiseSpeed", QString::number(exportData.getSpeed()));
    writer.writeAttribute("DepartureAirport", exportData.getDeparture());
    writer.writeAttribute("DestinationAirport", exportData.getDestination());
    writer.writeAttribute("AlternateAirport", exportData.getAlternate());
    writer.writeAttribute("Route", exportData.getRoute());
    writer.writeAttribute("Remarks", exportData.getRemarks());
    writer.writeAttribute("IsHeavy", exportData.isHeavy() ? "true" : "false");
    writer.writeAttribute("EquipmentPrefix", exportData.getEquipmentPrefix());
    writer.writeAttribute("EquipmentSuffix", exportData.getEquipmentSuffix());

    writer.writeAttribute("DepartureTime", exportData.getDepartureTime().toString("HHmm"));
    writer.writeAttribute("DepartureTimeAct", exportData.getDepartureTimeActual().isNull() ?
                          "0" : exportData.getDepartureTimeActual().toString("HHmm"));
    int enrouteHours = exportData.getEnrouteMinutes() / 60;
    writer.writeAttribute("EnrouteHours", QString::number(enrouteHours));
    writer.writeAttribute("EnrouteMinutes", QString::number(exportData.getEnrouteMinutes() - enrouteHours * 60));
    int enduranceHours = exportData.getEnduranceMinutes() / 60;
    writer.writeAttribute("FuelHours", QString::number(enduranceHours));
    writer.writeAttribute("FuelMinutes", QString::number(exportData.getEnduranceMinutes() - enduranceHours * 60));
    writer.writeAttribute("VoiceType", exportData.getVoiceType());

    writer.writeEndElement(); // FlightPlan
    writer.writeEndDocument();

    file.close();
    return true;
  }
  else
  {
    atools::gui::ErrorHandler(mainWindow).handleIOError(file, tr("While saving VFP file:"));
    return false;
  }
}

bool RouteExport::exportFlighplanAsIvap(const RouteExportData& exportData, const QString& filename,
                                        re::RouteExportType type)
{
  QFile file(filename);
  if(file.open(QFile::WriteOnly | QIODevice::Text))
  {
    // IvAp ===================================================
    // [FLIGHTPLAN]
    // ID=LU965
    // RULES=I
    // FLIGHTTYPE=S
    // NUMBER=1
    // ACTYPE=A306
    // WAKECAT=H
    // EQUIPMENT=SDE3FGHIRWY
    // TRANSPONDER=LB1
    // DEPICAO=KMFR
    // DEPTIME=1400
    // SPEEDTYPE=N
    // SPEED=0477
    // LEVELTYPE=F
    // LEVEL=310
    // ROUTE=BRUTE7 BRUTE V122 ACLOB DCT LEIFF DCT OLEBY DCT LMT DCT BIDDS DCT 4218N12112W DCT LKV V122 REO V113 BOI V4 LAR V118 CYS V138 SNY V6 IOW V8 BENKY BENKY5
    // DESTICAO=KORD
    // EET=0336
    // ALTICAO=KDTW
    // ALTICAO2=
    // OTHER=PBN/A1B1C1D1L1O1S1 DOF/200723 REG/N306SB EET/KZLC0026 KZDV0133 KZMP0222 KZAU0252 OPR/LU PER/C RMK/TCAS
    // ENDURANCE=0517
    // POB=275

    // X-IvAp ===================================================
    // [FLIGHTPLAN]
    // CALLSIGN=N9999
    // PIC=Pilot
    // FMCROUTE=
    // LIVERY=
    // AIRLINE=
    // SPEEDTYPE=N
    // POB=150
    // ENDURANCE=0531
    // OTHER=PBN/A1B1C1D1O1S1 DOF/200721 REG/N319SB OPR/LU PER/C RMK/TCAS
    // ALT2ICAO=
    // ALTICAO=KDTW
    // EET=0346
    // DESTICAO=KORD
    // ROUTE=BRUTE7 BRUTE V122 ACLOB DCT LEIFF DCT OLEBY DCT LMT DCT BIDDS DCT 4218N12112W DCT LKV V122 REO V113 BOI V4 LAR V118 CYS V138 SNY V6 IOW V8 BENKY BENKY5
    // LEVEL=350
    // LEVELTYPE=F
    // SPEED=0460
    // DEPTIME=1400
    // DEPICAO=KMFR
    // TRANSPONDER=LB1
    // EQUIPMENT=SDE3FGHIRWY
    // WAKECAT=M
    // ACTYPE=A319
    // NUMBER=1
    // FLIGHTTYPE=S
    // RULES=I

    QTextStream stream(&file);
    writeIvapLine(stream, "[FLIGHTPLAN]", type);

    // X-IvAp and IvAp idiotically use a slightly different format
    if(type == re::XIVAP)
    {
      writeIvapLine(stream, "CALLSIGN", exportData.getCallsign(), type);
      writeIvapLine(stream, "PIC", exportData.getPilotInCommand(), type);
      writeIvapLine(stream, "FMCROUTE", QString(), type);
      writeIvapLine(stream, "LIVERY", exportData.getLivery(), type);
      writeIvapLine(stream, "AIRLINE", exportData.getAirline(), type);
      writeIvapLine(stream, "SPEEDTYPE", "N", type);
      writeIvapLine(stream, "POB", exportData.getPassengers(), type);
      writeIvapLine(stream, "ENDURANCE", minToHourMinStr(exportData.getEnduranceMinutes()), type);
      writeIvapLine(stream, "OTHER", exportData.getRemarks(), type);
      writeIvapLine(stream, "ALT2ICAO", exportData.getAlternate2(), type);
      writeIvapLine(stream, "ALTICAO", exportData.getAlternate(), type);
      writeIvapLine(stream, "EET", minToHourMinStr(exportData.getEnrouteMinutes()), type);
      writeIvapLine(stream, "DESTICAO", exportData.getDestination(), type);
      writeIvapLine(stream, "ROUTE", exportData.getRoute(), type);
      writeIvapLine(stream, "LEVEL", exportData.getCruiseAltitude() / 100, type);
      writeIvapLine(stream, "LEVELTYPE", "F", type);
      writeIvapLine(stream, "SPEED", exportData.getSpeed(), type);
      writeIvapLine(stream, "DEPTIME", exportData.getDepartureTime().toString("HHmm"), type);
      writeIvapLine(stream, "DEPICAO", exportData.getDeparture(), type);
      writeIvapLine(stream, "TRANSPONDER", exportData.getTransponder(), type);
      writeIvapLine(stream, "EQUIPMENT", exportData.getEquipment(), type);
      writeIvapLine(stream, "WAKECAT", exportData.getWakeCategory(), type);
      writeIvapLine(stream, "ACTYPE", exportData.getAircraftType(), type);
      writeIvapLine(stream, "NUMBER", "1", type);
      writeIvapLine(stream, "FLIGHTTYPE", exportData.getFlightType(), type);
      writeIvapLine(stream, "RULES", exportData.getFlightRules(), type);
    }
    else
    {
      writeIvapLine(stream, "ID", exportData.getCallsign(), type);
      writeIvapLine(stream, "RULES", exportData.getFlightRules(), type);
      writeIvapLine(stream, "FLIGHTTYPE", exportData.getFlightType(), type);
      writeIvapLine(stream, "NUMBER", "1", type);
      writeIvapLine(stream, "ACTYPE", exportData.getAircraftType(), type);
      writeIvapLine(stream, "WAKECAT", exportData.getWakeCategory(), type);
      writeIvapLine(stream, "EQUIPMENT", exportData.getEquipment(), type);
      writeIvapLine(stream, "TRANSPONDER", exportData.getTransponder(), type);
      writeIvapLine(stream, "DEPICAO", exportData.getDeparture(), type);
      writeIvapLine(stream, "DEPTIME", exportData.getDepartureTime().toString("HHmm"), type);
      writeIvapLine(stream, "SPEEDTYPE", "N", type);
      writeIvapLine(stream, "SPEED", exportData.getSpeed(), type);
      writeIvapLine(stream, "LEVELTYPE", "F", type);
      writeIvapLine(stream, "LEVEL", exportData.getCruiseAltitude() / 100, type);
      writeIvapLine(stream, "ROUTE", exportData.getRoute(), type);
      writeIvapLine(stream, "DESTICAO", exportData.getDestination(), type);
      writeIvapLine(stream, "EET", minToHourMinStr(exportData.getEnrouteMinutes()), type);
      writeIvapLine(stream, "ALTICAO", exportData.getAlternate(), type);
      writeIvapLine(stream, "ALTICAO2", exportData.getAlternate2(), type);
      writeIvapLine(stream, "OTHER", exportData.getRemarks(), type);
      writeIvapLine(stream, "ENDURANCE", minToHourMinStr(exportData.getEnduranceMinutes()), type);
      writeIvapLine(stream, "POB", exportData.getPassengers(), type);
    }

    file.close();
    return true;
  }
  else
  {
    atools::gui::ErrorHandler(mainWindow).handleIOError(file, tr("While saving FPL file:"));
    return false;
  }
}

bool RouteExport::exportFlighplan(const QString& filename, rf::RouteAdjustOptions options,
                                  std::function<void(const atools::fs::pln::Flightplan& plan,
                                                     const QString& file)> exportFunc)
{
  try
  {
    exportFunc(buildAdjustedRoute(options).getFlightplan(), filename);
  }
  catch(atools::Exception& e)
  {
    atools::gui::ErrorHandler(mainWindow).handleException(e);
    return false;
  }
  catch(...)
  {
    atools::gui::ErrorHandler(mainWindow).handleUnknownException();
    return false;
  }
  return true;
}

bool RouteExport::exportFlighplanAsCorteIn(const QString& filename)
{
  qDebug() << Q_FUNC_INFO << filename;
  QString txt = RouteStringWriter().createStringForRoute(
    buildAdjustedRoute(rf::DEFAULT_OPTS), 0.f,
    rs::DCT | rs::NO_FINAL_DCT | rs::START_AND_DEST | rs::SID_STAR | rs::SID_STAR_SPACE |
    rs::RUNWAY /*| rs::APPROACH unreliable */ | rs::FLIGHTLEVEL);

  const atools::fs::pln::Flightplan& flightplan = NavApp::getRouteConst().getFlightplan();

  QSet<QString> routeNames;
  QFile file(filename);
  if(file.open(QFile::ReadOnly | QIODevice::Text))
  {
    while(!file.atEnd())
    {
      QString line = file.readLine().toUpper().simplified();
      if(line.isEmpty())
        continue;

      // RTE LHRAMS01 EGLL 27L BPK7G BPK DCT CLN UL620 REDFA REDF1A EHAM I18R SPL CI30 FL250
      routeNames.insert(line.section(" ", 1, 1));
    }
    file.close();
  }
  else
  {
    atools::gui::ErrorHandler(mainWindow).handleIOError(file, tr("While reading corte.in file:"));
    return false;
  }

  QString name = flightplan.getDepartureIdent() + flightplan.getDestinationIdent();

  // Find a unique name between all loaded
  int i = 1;
  while(routeNames.contains(name) && i < 99)
  {
    QString str = name.left(6);
    name = QString("%1%2").arg(str).arg(i++, 8 - str.size(), 10, QChar('0'));
  }

  txt.prepend(QString("RTE %1 ").arg(name));

  // Check if we have to insert an endl first
  bool endsWithEol = atools::fileEndsWithEol(filename);

  // Append string to file
  if(file.open(QFile::Append | QIODevice::Text))
  {
    QTextStream stream(&file);

    if(!endsWithEol)
      stream << endl;
    stream << txt;
    file.close();
    return true;
  }
  else
  {
    atools::gui::ErrorHandler(mainWindow).handleIOError(file, tr("While saving to corte.in file:"));
    return false;
  }
}

bool RouteExport::exportFlighplanAsProSim(const QString& filename)
{
  qDebug() << Q_FUNC_INFO << filename;

  // <?xml version="1.0" encoding="UTF-8"?>
  // <companyroutes>
  // <route name="KDSMKOKC">KDSM DSM J25 TUL KOKC </route>
  // <route name="EDDHEDDS">EDDH IDEKO Y900 TIMEN UL126 WRB UN850 KRH T128 BADSO EDDS</route>
  // <route name="EDDSEDDH">EDDS KRH UZ210 NOSPA EDDL</route>
  // </companyroutes>

  // Read the XML file and keep all routes ===========================================
  QVector<std::pair<QString, QString> > routes;
  QSet<QString> routeNames;

  QFile file(filename);
  if(file.exists() && file.size() > 0)
  {
    if(file.open(QFile::ReadOnly | QIODevice::Text))
    {
      QXmlStreamReader reader(&file);

      while(!reader.atEnd())
      {
        if(reader.error() != QXmlStreamReader::NoError)
          throw atools::Exception("Error reading \"" + filename + "\": " + reader.errorString());

        QXmlStreamReader::TokenType token = reader.readNext();

        if(token == QXmlStreamReader::StartElement && reader.name() == "route")
        {
          QString name = reader.attributes().value("name").toString();
          QString route = reader.readElementText();
          routes.append(std::make_pair(name, route));
          routeNames.insert(name);
        }
      }
      file.close();
    }
    else
    {
      atools::gui::ErrorHandler(mainWindow).handleIOError(file, tr("While reading from companyroutes.xml file:"));
      return false;
    }
  }

  // Create maximum of two backup files
  QString backupFile = filename + "_lnm_backup";
  atools::io::FileRoller roller(1);
  roller.rollFile(backupFile);

  // Copy file to backup before opening
  bool result = QFile(filename).copy(backupFile);
  qDebug() << Q_FUNC_INFO << "Copied" << filename << "to" << backupFile << result;

  // Create route string
  QString route =
    RouteStringWriter().createStringForRoute(buildAdjustedRoute(rf::DEFAULT_OPTS), 0.f, rs::START_AND_DEST);
  QString name = buildDefaultFilenameShort(QString(), QString());

  // Find a unique name between all loaded
  QString newname = name;
  int i = 1;
  while(routeNames.contains(newname) && i < 99)
    newname = QString("%1%2").arg(name).arg(i++, 2, 10, QChar('0'));

  // Add new route
  routes.append(std::make_pair(newname, route));

  // Save and overwrite new file ====================================================
  if(file.open(QFile::WriteOnly | QIODevice::Text))
  {
    QXmlStreamWriter writer(&file);
    writer.setAutoFormatting(true);
    writer.setAutoFormattingIndent(2);
    writer.writeStartDocument();

    writer.writeStartElement("companyroutes");

    for(const std::pair<QString, QString>& entry : routes)
    {
      // <route name="KDSMKOKC">KDSM DSM J25 TUL KOKC </route>
      writer.writeStartElement("route");
      writer.writeAttribute("name", entry.first);
      writer.writeCharacters(entry.second);
      writer.writeEndElement(); // route
    }

    writer.writeEndElement(); // companyroutes
    writer.writeEndDocument();
    file.close();
    return true;
  }
  else
  {
    atools::gui::ErrorHandler(mainWindow).handleIOError(file, tr("While saving to companyroutes.xml file:"));
    return false;
  }
}

bool RouteExport::exportFlightplanAsGpx(const QString& filename)
{
  qDebug() << Q_FUNC_INFO << filename;

  try
  {
    atools::geo::LineString track;
    QVector<quint32> timestamps;
    NavApp::getAircraftTrack().convert(&track, &timestamps);
    FlightplanIO().saveGpx(buildAdjustedRoute(rf::DEFAULT_OPTS_GPX).getFlightplan(), filename, track, timestamps,
                           static_cast<int>(NavApp::getRouteConst().getCruisingAltitudeFeet()));
  }
  catch(atools::Exception& e)
  {
    atools::gui::ErrorHandler(mainWindow).handleException(e);
    return false;
  }
  catch(...)
  {
    atools::gui::ErrorHandler(mainWindow).handleUnknownException();
    return false;
  }
  return true;
}

Route RouteExport::buildAdjustedRoute(rf::RouteAdjustOptions options)
{
  return buildAdjustedRoute(NavApp::getRoute(), options);
}

Route RouteExport::buildAdjustedRoute(const Route& route, rf::RouteAdjustOptions options)
{
  if(NavApp::getMainUi()->actionRouteSaveApprWaypoints->isChecked())
    options |= rf::SAVE_APPROACH_WP;
  if(NavApp::getMainUi()->actionRouteSaveSidStarWaypoints->isChecked())
    options |= rf::SAVE_SIDSTAR_WP;
  if(NavApp::getMainUi()->actionRouteSaveAirwayWaypoints->isChecked())
    options |= rf::SAVE_AIRWAY_WP;

  Route rt = route.updatedAltitudes().adjustedToOptions(options);

  // Update airway structures
  rt.updateAirwaysAndAltitude(false /* adjustRouteAltitude */);

  return rt;
}

QString RouteExport::minToHourMinStr(int minutes)
{
  int enrouteHours = minutes / 60;
  return QString("%1%2").arg(enrouteHours, 2, 10, QChar('0')).arg(minutes - enrouteHours * 60, 2, 10, QChar('0'));
}

void RouteExport::writeIvapLine(QTextStream& stream, const QString& string, re::RouteExportType type)
{
  stream << string;
  if(type == re::XIVAP)
    stream << "\r\r\n";
  else if(type == re::IVAP)
    stream << "\r\n";
}

void RouteExport::writeIvapLine(QTextStream& stream, const QString& key, const QString& value, re::RouteExportType type)
{
  stream << key << "=" << value;
  if(type == re::XIVAP)
    stream << "\r\r\n";
  else if(type == re::IVAP)
    stream << "\r\n";
}

void RouteExport::writeIvapLine(QTextStream& stream, const QString& key, int value, re::RouteExportType type)
{
  stream << key << "=" << value;
  if(type == re::XIVAP)
    stream << "\r\r\n";
  else if(type == re::IVAP)
    stream << "\r\n";
}
