/*****************************************************************************
* Copyright 2015-2019 Alexander Barthel alex@littlenavmap.org
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

#include "gui/mainwindow.h"

#include "common/constants.h"
#include "common/proctypes.h"
#include "navapp.h"
#include "atools.h"
#include "common/mapcolors.h"
#include "gui/stylehandler.h"
#include "util/htmlbuilder.h"
#include "fs/common/morareader.h"
#include "gui/application.h"
#include "route/routealtitude.h"
#include "weather/weatherreporter.h"
#include "connect/connectclient.h"
#include "common/elevationprovider.h"
#include "db/databasemanager.h"
#include "gui/dialog.h"
#include "gui/errorhandler.h"
#include "gui/helphandler.h"
#include "gui/itemviewzoomhandler.h"
#include "gui/translator.h"
#include "gui/widgetstate.h"
#include "info/infocontroller.h"
#include "logging/logginghandler.h"
#include "logging/loggingguiabort.h"
#include "query/airportquery.h"
#include "mapgui/mapwidget.h"
#include "mapgui/aprongeometrycache.h"
#include "profile/profilewidget.h"
#include "route/routecontroller.h"
#include "gui/filehistoryhandler.h"
#include "search/airportsearch.h"
#include "search/userdatasearch.h"
#include "search/navsearch.h"
#include "mapgui/maplayersettings.h"
#include "search/searchcontroller.h"
#include "settings/settings.h"
#include "options/optionsdialog.h"
#include "print/printsupport.h"
#include "exception.h"
#include "route/routestringdialog.h"
#include "common/unit.h"
#include "fs/pln/flightplanio.h"
#include "query/procedurequery.h"
#include "search/proceduresearch.h"
#include "gui/airspacetoolbarhandler.h"
#include "userdata/userdatacontroller.h"
#include "online/onlinedatacontroller.h"
#include "search/onlineclientsearch.h"
#include "search/onlinecentersearch.h"
#include "search/onlineserversearch.h"
#include "route/routeexport.h"
#include "query/airspacequery.h"
#include "gui/timedialog.h"
#include "util/version.h"
#include "perf/aircraftperfcontroller.h"
#include "fs/perf/aircraftperf.h"

#include <marble/LegendWidget.h>
#include <marble/MarbleAboutDialog.h>
#include <marble/MarbleModel.h>

#include <QDebug>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QFileInfo>
#include <QScreen>
#include <QWindow>
#include <QDesktopWidget>
#include <QDir>
#include <QFileInfoList>
#include <QSslSocket>
#include <QEvent>
#include <QMimeData>
#include <QClipboard>

#include "ui_mainwindow.h"

static const int WEATHER_UPDATE_MS = 15000;

// All known map themes
static const QStringList STOCK_MAP_THEMES({"clouds", "hillshading", "openstreetmap", "openstreetmaproads",
                                           "openstreetmaproadshs", "opentopomap", "plain", "political",
                                           "srtm", "srtm2", "stamenterrain", "cartodark", "cartolight"});

using namespace Marble;
using atools::settings::Settings;
using atools::gui::FileHistoryHandler;
using atools::gui::MapPosHistory;
using atools::gui::HelpHandler;

MainWindow::MainWindow()
  : QMainWindow(nullptr), ui(new Ui::MainWindow)
{
  qDebug() << "MainWindow constructor";

  aboutMessage =
    QObject::tr("<p>is a free open source flight planner, navigation tool, moving map, "
                  "airport search and airport information system for X-Plane 11, Flight Simulator X and Prepar3D.</p>"
                  "<p>"
                    "<b>"
                      "If you would like to show your appreciation you can donate&nbsp;"
                      "<a href=\"%1\">here"
                      "</a>."
                    "</b>"
                  "</p>"
                  "<p>This software is licensed under "
                    "<a href=\"http://www.gnu.org/licenses/gpl-3.0\">GPL3"
                    "</a> or any later version."
                  "</p>"
                  "<p>The source code for this application is available at "
                    "<a href=\"https://github.com/albar965\">Github"
                    "</a>."
                  "</p>"
                  "<p>More about my projects at "
                    "<a href=\"https://www.littlenavmap.org\">www.littlenavmap.org"
                    "</a>."
                  "</p>"
                  "<p>"
                    "<b>Copyright 2015-2019 Alexander Barthel"
                    "</b>"
                  "</p>").arg(lnm::helpDonateUrl);

  // Show a dialog on fatal log events like asserts
  atools::logging::LoggingGuiAbortHandler::setGuiAbortFunction(this);

  try
  {
    // Have to handle exceptions here since no message handler is active yet and no
    // atools::Application method can catch it

    ui->setupUi(this);
    setAcceptDrops(true);

    dialog = new atools::gui::Dialog(this);
    errorHandler = new atools::gui::ErrorHandler(this);
    helpHandler = new atools::gui::HelpHandler(this, aboutMessage, GIT_REVISION);

    marbleAbout = new Marble::MarbleAboutDialog(this);
    marbleAbout->setApplicationTitle(QApplication::applicationName());

    currentWeatherContext = new map::WeatherContext;

    routeExport = new RouteExport(this);

    qDebug() << "MainWindow Creating OptionsDialog";
    optionsDialog = new OptionsDialog(this);
    // Has to load the state now so options are available for all controller and manager classes
    optionsDialog->restoreState();

    // Setup central widget ==================================================
    // Set one pixel fixed width
    // QWidget *centralWidget = new QWidget(this);
    // centralWidget->setWindowFlags(windowFlags() & ~(Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus));
    // centralWidget->setMinimumSize(1, 1);
    // centralWidget->resize(1, 1);
    // centralWidget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    // centralWidget->hide(); // Potentially messes up docking windows (i.e. Profile dock cannot be shrinked) in certain configurations.
    // setCentralWidget(centralWidget);
    if(OptionData::instance().getFlags2() & opts::MAP_ALLOW_UNDOCK)
      centralWidget()->hide();

    setupUi();

    // Load all map feature colors
    mapcolors::syncColors();

    Unit::init();

    map::updateUnits();

    // Remember original title
    mainWindowTitle = windowTitle();

    // Prepare database and queries
    qDebug() << "MainWindow Creating DatabaseManager";

    NavApp::init(this);

    NavApp::getStyleHandler()->insertMenuItems(ui->menuWindowStyle);
    NavApp::getStyleHandler()->restoreState();
    mapcolors::init();

    // Add actions for flight simulator database switch in main menu
    NavApp::getDatabaseManager()->insertSimSwitchActions();

    qDebug() << "MainWindow Creating WeatherReporter";
    weatherReporter = new WeatherReporter(this, NavApp::getCurrentSimulatorDb());

    qDebug() << "MainWindow Creating FileHistoryHandler for flight plans";
    routeFileHistory = new FileHistoryHandler(this, lnm::ROUTE_FILENAMESRECENT, ui->menuRecentRoutes,
                                              ui->actionRecentRoutesClear);

    qDebug() << "MainWindow Creating RouteController";
    routeController = new RouteController(this, ui->tableViewRoute);

    qDebug() << "MainWindow Creating FileHistoryHandler for KML files";
    kmlFileHistory = new FileHistoryHandler(this, lnm::ROUTE_FILENAMESKMLRECENT, ui->menuRecentKml,
                                            ui->actionClearKmlMenu);

    // Create map widget and replace dummy widget in window
    qDebug() << "MainWindow Creating MapWidget";
    mapWidget = new MapWidget(this);
    if(OptionData::instance().getFlags2() & opts::MAP_ALLOW_UNDOCK)
    {
      ui->verticalLayoutMap->replaceWidget(ui->widgetDummyMap, mapWidget);
      ui->dockWidgetMap->show();
    }
    else
    {
      setCentralWidget(mapWidget);
      ui->dockWidgetMap->hide();
    }

    NavApp::initElevationProvider();

    // Create elevation profile widget and replace dummy widget in window
    qDebug() << "MainWindow Creating ProfileWidget";
    profileWidget = new ProfileWidget(ui->scrollAreaProfile->viewport());
    ui->scrollAreaProfile->setWidget(profileWidget);
    profileWidget->show();

    // Have to create searches in the same order as the tabs
    qDebug() << "MainWindow Creating SearchController";
    searchController = new SearchController(this, ui->tabWidgetSearch);
    searchController->createAirportSearch(ui->tableViewAirportSearch);
    searchController->createNavSearch(ui->tableViewNavSearch);

    searchController->createProcedureSearch(ui->treeWidgetApproachSearch);

    searchController->createUserdataSearch(ui->tableViewUserdata);

    searchController->createOnlineClientSearch(ui->tableViewOnlineClientSearch);
    searchController->createOnlineCenterSearch(ui->tableViewOnlineCenterSearch);
    searchController->createOnlineServerSearch(ui->tableViewOnlineServerSearch);

    qDebug() << "MainWindow Creating InfoController";
    infoController = new InfoController(this);

    qDebug() << "MainWindow Creating InfoController";
    airspaceHandler = new AirspaceToolBarHandler(this);
    airspaceHandler->createToolButtons();

    qDebug() << "MainWindow Creating PrintSupport";
    printSupport = new PrintSupport(this);

    qDebug() << "MainWindow Connecting slots";
    connectAllSlots();
    NavApp::getAircraftPerfController()->connectAllSlots();

    // Add user defined points toolbar button and submenu items
    NavApp::getUserdataController()->addToolbarButton();

    qDebug() << "MainWindow Reading settings";
    restoreStateMain();

    updateActionStates();
    updateMarkActionStates();
    updateHighlightActionStates();

    airspaceHandler->updateButtonsAndActions();
    updateOnlineActionStates();

    qDebug() << "MainWindow Setting theme";
    changeMapTheme();

    qDebug() << "MainWindow Setting projection";
    mapWidget->setProjection(mapProjectionComboBox->currentData().toInt());

    // Wait until everything is set up and update map
    updateMapObjectsShown();

    profileWidget->updateProfileShowFeatures();

    // Initialize the X-Plane apron geometry cache
    NavApp::getApronGeometryCache()->setViewportParams(NavApp::getMapWidget()->viewport());

    loadNavmapLegend();
    updateLegend();
    updateWindowTitle();

    clockTimer.setInterval(1000);
    connect(&clockTimer, &QTimer::timeout, this, &MainWindow::updateClock);
    clockTimer.start();

    qDebug() << "MainWindow Constructor done";
  }
  // Exit application if something goes wrong
  catch(atools::Exception& e)
  {
    ATOOLS_HANDLE_EXCEPTION(e);
  }
  catch(...)
  {
    ATOOLS_HANDLE_UNKNOWN_EXCEPTION;
  }
}

MainWindow::~MainWindow()
{
  qDebug() << Q_FUNC_INFO;

  clockTimer.stop();

  NavApp::setShuttingDown(true);

  weatherUpdateTimer.stop();

  // Close all queries
  preDatabaseLoad();

  qDebug() << Q_FUNC_INFO << "delete routeController";
  delete routeController;
  qDebug() << Q_FUNC_INFO << "delete searchController";
  delete searchController;
  qDebug() << Q_FUNC_INFO << "delete weatherReporter";
  delete weatherReporter;
  qDebug() << Q_FUNC_INFO << "delete profileWidget";
  delete profileWidget;
  qDebug() << Q_FUNC_INFO << "delete marbleAbout";
  delete marbleAbout;
  qDebug() << Q_FUNC_INFO << "delete infoController";
  delete infoController;
  qDebug() << Q_FUNC_INFO << "delete printSupport";
  delete printSupport;
  qDebug() << Q_FUNC_INFO << "delete airspaceHandler";
  delete airspaceHandler;
  qDebug() << Q_FUNC_INFO << "delete routeFileHistory";
  delete routeFileHistory;
  qDebug() << Q_FUNC_INFO << "delete kmlFileHistory";
  delete kmlFileHistory;
  qDebug() << Q_FUNC_INFO << "delete optionsDialog";
  delete optionsDialog;
  qDebug() << Q_FUNC_INFO << "delete mapWidget";
  delete mapWidget;
  qDebug() << Q_FUNC_INFO << "delete ui";
  delete ui;
  qDebug() << Q_FUNC_INFO << "delete dialog";
  delete dialog;
  qDebug() << Q_FUNC_INFO << "delete errorHandler";
  delete errorHandler;
  qDebug() << Q_FUNC_INFO << "delete helpHandler";
  delete helpHandler;
  qDebug() << Q_FUNC_INFO << "delete actionGroupMapProjection";
  delete actionGroupMapProjection;
  qDebug() << Q_FUNC_INFO << "delete actionGroupMapTheme";
  delete actionGroupMapTheme;
  qDebug() << Q_FUNC_INFO << "delete actionGroupMapSunShading";
  delete actionGroupMapSunShading;
  qDebug() << Q_FUNC_INFO << "delete actionGroupMapWeatherSource";
  delete actionGroupMapWeatherSource;

  qDebug() << Q_FUNC_INFO << "delete currentWeatherContext";
  delete currentWeatherContext;

  qDebug() << Q_FUNC_INFO << "delete routeExport";
  delete routeExport;

  qDebug() << Q_FUNC_INFO << "NavApplication::deInit()";
  NavApp::deInit();

  qDebug() << Q_FUNC_INFO << "Unit::deInit()";
  Unit::deInit();

  // Delete settings singleton
  qDebug() << Q_FUNC_INFO << "Settings::shutdown()";
  Settings::shutdown();
  atools::gui::Translator::unload();

  atools::logging::LoggingGuiAbortHandler::resetGuiAbortFunction();

  qDebug() << "MainWindow destructor about to shut down logging";
  atools::logging::LoggingHandler::shutdown();
}

void MainWindow::updateMap() const
{
  mapWidget->update();
}

void MainWindow::updateClock() const
{
  timeLabel->setText(QDateTime::currentDateTimeUtc().toString("d   HH:mm:ss Z"));
  timeLabel->setToolTip(tr("Day of month and UTC time.\n%1\nLocal: %2")
                        .arg(QDateTime::currentDateTimeUtc().toString())
                        .arg(QDateTime::currentDateTime().toString()));
}

/* Show map legend and bring information dock to front */
void MainWindow::showNavmapLegend()
{
  if(!legendFile.isEmpty() && QFile::exists(legendFile))
  {
    ui->dockWidgetLegend->show();
    ui->dockWidgetLegend->raise();
    ui->tabWidgetLegend->setCurrentIndex(0);
    setStatusMessage(tr("Opened navigation map legend."));
  }
  else
  {
    // URL is empty loading failed - show it in browser
    helpHandler->openHelpUrlWeb(this, lnm::helpOnlineLegendUrl, lnm::helpLanguageOnline());
    setStatusMessage(tr("Opened map legend in browser."));
  }
}

/* Load the navmap legend into the text browser */
void MainWindow::loadNavmapLegend()
{
  qDebug() << Q_FUNC_INFO;

  legendFile = HelpHandler::getHelpFile(lnm::helpLegendLocalFile,
                                        OptionData::instance().getFlags() & opts::GUI_OVERRIDE_LANGUAGE);
  qDebug() << "legendUrl" << legendFile;

  QString legendText;
  QFile legend(legendFile);
  if(legend.open(QIODevice::ReadOnly))
  {
    QTextStream stream(&legend);
    stream.setCodec("UTF-8");
    legendText.append(stream.readAll());

    QString searchPath = QCoreApplication::applicationDirPath() + QDir::separator() + "help";
    ui->textBrowserLegendNavInfo->setSearchPaths({searchPath});
    ui->textBrowserLegendNavInfo->setText(legendText);
  }
  else
  {
    qWarning() << "Error opening legend" << legendFile << legend.errorString();
    legendFile.clear();
  }
}

/* Check manually for updates as triggered by the action */
void MainWindow::checkForUpdates()
{
  NavApp::checkForUpdates(OptionData::instance().getUpdateChannels(), true /* manually triggered */);
}

void MainWindow::showOnlineHelp()
{
  HelpHandler::openHelpUrlWeb(this, lnm::helpOnlineUrl, lnm::helpLanguageOnline());
}

void MainWindow::showOnlineTutorials()
{
  HelpHandler::openHelpUrlWeb(this, lnm::helpOnlineTutorialsUrl, lnm::helpLanguageOnline());
}

void MainWindow::showDonationPage()
{
  HelpHandler::openUrlWeb(this, lnm::helpDonateUrl);
}

void MainWindow::showFaqPage()
{
  HelpHandler::openUrlWeb(this, lnm::helpFaqUrl);
}

void MainWindow::showOfflineHelp()
{
  HelpHandler::openFile(this, HelpHandler::getHelpFile(lnm::helpOfflineFile,
                                                       OptionData::instance().getFlags() &
                                                       opts::GUI_OVERRIDE_LANGUAGE));
}

/* Show marble legend */
void MainWindow::showMapLegend()
{
  ui->dockWidgetLegend->show();
  ui->dockWidgetLegend->raise();
  ui->tabWidgetLegend->setCurrentIndex(1);
  setStatusMessage(tr("Opened map legend."));
}

/* User clicked "show in browser" in legend */
void MainWindow::legendAnchorClicked(const QUrl& url)
{
  if(url.scheme() == "lnm" && url.host() == "legend")
    HelpHandler::openHelpUrlWeb(this, lnm::helpOnlineUrl + "LEGEND.html", lnm::helpLanguageOnline());
  else
    HelpHandler::openUrl(this, url);

  setStatusMessage(tr("Opened legend link in browser."));
}

void MainWindow::scaleToolbar(QToolBar *toolbar, float scale)
{
  QSizeF size = toolbar->iconSize();
  size *= scale;
  toolbar->setIconSize(size.toSize());
}

/* Set up own UI elements that cannot be created in designer */
void MainWindow::setupUi()
{
  // Reduce large icons on mac
#if defined(Q_OS_MACOS)
  scaleToolbar(ui->toolBarMain, 0.72f);
  scaleToolbar(ui->toolBarMap, 0.72f);
  scaleToolbar(ui->toolbarMapOptions, 0.72f);
  scaleToolbar(ui->toolBarRoute, 0.72f);
  scaleToolbar(ui->toolBarAirspaces, 0.72f);
  scaleToolbar(ui->toolBarView, 0.72f);
#endif

  ui->toolbarMapOptions->addSeparator();

  // Projection combo box
  mapProjectionComboBox = new QComboBox(this);
  mapProjectionComboBox->setObjectName("mapProjectionComboBox");
  QString helpText = tr("Select map projection");
  mapProjectionComboBox->setToolTip(helpText);
  mapProjectionComboBox->setStatusTip(helpText);
  mapProjectionComboBox->addItem(tr("Mercator"), Marble::Mercator);
  mapProjectionComboBox->addItem(tr("Spherical"), Marble::Spherical);
  ui->toolbarMapOptions->addWidget(mapProjectionComboBox);

  // Projection menu items
  actionGroupMapProjection = new QActionGroup(ui->menuViewProjection);
  ui->actionMapProjectionMercator->setActionGroup(actionGroupMapProjection);
  ui->actionMapProjectionSpherical->setActionGroup(actionGroupMapProjection);

  // Theme combo box
  mapThemeComboBox = new QComboBox(this);
  mapThemeComboBox->setObjectName("mapThemeComboBox");
  helpText = tr("Select map theme");
  mapThemeComboBox->setToolTip(helpText);
  mapThemeComboBox->setStatusTip(helpText);
  // Item order has to match MapWidget::MapThemeComboIndex
  mapThemeComboBox->addItem(tr("OpenStreetMap"), "earth/openstreetmap/openstreetmap.dgml");
  mapThemeComboBox->addItem(tr("OpenMapSurfer"), "earth/openstreetmaproads/openstreetmaproads.dgml");
  mapThemeComboBox->addItem(tr("OpenTopoMap"), "earth/opentopomap/opentopomap.dgml");
  mapThemeComboBox->addItem(tr("Stamen Terrain"), "earth/stamenterrain/stamenterrain.dgml");
  mapThemeComboBox->addItem(tr("CARTO Light"), "earth/cartolight/cartolight.dgml");
  mapThemeComboBox->addItem(tr("CARTO Dark"), "earth/cartodark/cartodark.dgml");
  mapThemeComboBox->addItem(tr("Simple (Offline)"), "earth/political/political.dgml");
  mapThemeComboBox->addItem(tr("Plain (Offline)"), "earth/plain/plain.dgml");
  mapThemeComboBox->addItem(tr("Atlas (Offline)"), "earth/srtm/srtm.dgml");
  ui->toolbarMapOptions->addWidget(mapThemeComboBox);

  // Sun shading sub menu
  actionGroupMapSunShading = new QActionGroup(ui->menuSunShading);
  actionGroupMapSunShading->addAction(ui->actionMapShowSunShadingSimulatorTime);
  actionGroupMapSunShading->addAction(ui->actionMapShowSunShadingRealTime);
  actionGroupMapSunShading->addAction(ui->actionMapShowSunShadingUserTime);

  // Weather source sub menu
  actionGroupMapWeatherSource = new QActionGroup(ui->menuMapShowAirportWeatherSource);
  actionGroupMapWeatherSource->addAction(ui->actionMapShowWeatherSimulator);
  actionGroupMapWeatherSource->addAction(ui->actionMapShowWeatherActiveSky);
  actionGroupMapWeatherSource->addAction(ui->actionMapShowWeatherNoaa);
  actionGroupMapWeatherSource->addAction(ui->actionMapShowWeatherVatsim);
  actionGroupMapWeatherSource->addAction(ui->actionMapShowWeatherIvao);

  // Theme menu items
  actionGroupMapTheme = new QActionGroup(ui->menuViewTheme);
  ui->actionMapThemeOpenStreetMap->setActionGroup(actionGroupMapTheme);
  ui->actionMapThemeOpenStreetMap->setData(map::OPENSTREETMAP);

  ui->actionMapThemeOpenStreetMapRoads->setActionGroup(actionGroupMapTheme);
  ui->actionMapThemeOpenStreetMapRoads->setData(map::OPENSTREETMAPROADS);

  ui->actionMapThemeOpenTopoMap->setActionGroup(actionGroupMapTheme);
  ui->actionMapThemeOpenTopoMap->setData(map::OPENTOPOMAP);

  ui->actionMapThemeStamenTerrain->setActionGroup(actionGroupMapTheme);
  ui->actionMapThemeStamenTerrain->setData(map::STAMENTERRAIN);

  ui->actionMapThemeCartoLight->setActionGroup(actionGroupMapTheme);
  ui->actionMapThemeCartoLight->setData(map::CARTOLIGHT);

  ui->actionMapThemeCartoDark->setActionGroup(actionGroupMapTheme);
  ui->actionMapThemeCartoDark->setData(map::CARTODARK);

  ui->actionMapThemeSimple->setActionGroup(actionGroupMapTheme);
  ui->actionMapThemeSimple->setData(map::SIMPLE);

  ui->actionMapThemePlain->setActionGroup(actionGroupMapTheme);
  ui->actionMapThemePlain->setData(map::PLAIN);

  ui->actionMapThemeAtlas->setActionGroup(actionGroupMapTheme);
  ui->actionMapThemeAtlas->setData(map::ATLAS);

  // Add any custom map themes that were found besides the stock themes
  QFileInfoList customDgmlFiles;
  findCustomMaps(customDgmlFiles);

  if(!customDgmlFiles.isEmpty())
    ui->menuViewTheme->addSeparator();

  for(int i = 0; i < customDgmlFiles.size(); i++)
  {
    const QFileInfo& dgmlFile = customDgmlFiles.at(i);
    QString nameShort = tr("* %1").arg(dgmlFile.baseName());

    // Add item to combo box in toolbar
    mapThemeComboBox->addItem(nameShort, dgmlFile.filePath());

    // Create action for map/theme submenu
    QString name = tr("Custom (%1)").arg(dgmlFile.baseName());
    QString helptext = tr("Custom map theme (%1)").arg(dgmlFile.baseName());
    QAction *action = ui->menuViewTheme->addAction(name);
    action->setCheckable(true);
    action->setToolTip(helptext);
    action->setStatusTip(helptext);
    action->setActionGroup(actionGroupMapTheme);
    action->setData(map::CUSTOM + i);

    // Add to list for connect signal etc.
    customMapThemeMenuActions.append(action);
  }

  // Update dock widget actions with tooltip, status tip and keypress
  ui->dockWidgetSearch->toggleViewAction()->setIcon(QIcon(":/littlenavmap/resources/icons/searchdock.svg"));
  ui->dockWidgetSearch->toggleViewAction()->setShortcut(QKeySequence(tr("Alt+1")));
  ui->dockWidgetSearch->toggleViewAction()->setToolTip(tr("Open or show the %1 dock window").
                                                       arg(ui->dockWidgetSearch->windowTitle()));
  ui->dockWidgetSearch->toggleViewAction()->setStatusTip(ui->dockWidgetSearch->toggleViewAction()->toolTip());

  ui->dockWidgetRoute->toggleViewAction()->setIcon(QIcon(":/littlenavmap/resources/icons/routedock.svg"));
  ui->dockWidgetRoute->toggleViewAction()->setShortcut(QKeySequence(tr("Alt+2")));
  ui->dockWidgetRoute->toggleViewAction()->setToolTip(tr("Open or show the %1 dock window").
                                                      arg(ui->dockWidgetRoute->windowTitle()));
  ui->dockWidgetRoute->toggleViewAction()->setStatusTip(ui->dockWidgetRoute->toggleViewAction()->toolTip());

  ui->dockWidgetInformation->toggleViewAction()->setIcon(QIcon(":/littlenavmap/resources/icons/infodock.svg"));
  ui->dockWidgetInformation->toggleViewAction()->setShortcut(QKeySequence(tr("Alt+3")));
  ui->dockWidgetInformation->toggleViewAction()->setToolTip(tr("Open or show the %1 dock window").
                                                            arg(ui->dockWidgetInformation->windowTitle().
                                                                toLower()));
  ui->dockWidgetInformation->toggleViewAction()->setStatusTip(ui->dockWidgetInformation->toggleViewAction()->toolTip());

  ui->dockWidgetProfile->toggleViewAction()->setIcon(QIcon(":/littlenavmap/resources/icons/profiledock.svg"));
  ui->dockWidgetProfile->toggleViewAction()->setShortcut(QKeySequence(tr("Alt+4")));
  ui->dockWidgetProfile->toggleViewAction()->setToolTip(tr("Open or show the %1 dock window").
                                                        arg(ui->dockWidgetProfile->windowTitle()));
  ui->dockWidgetProfile->toggleViewAction()->setStatusTip(ui->dockWidgetProfile->toggleViewAction()->toolTip());

  ui->dockWidgetAircraft->toggleViewAction()->setIcon(QIcon(":/littlenavmap/resources/icons/aircraftdock.svg"));
  ui->dockWidgetAircraft->toggleViewAction()->setShortcut(QKeySequence(tr("Alt+5")));
  ui->dockWidgetAircraft->toggleViewAction()->setToolTip(tr("Open or show the %1 dock window").
                                                         arg(ui->dockWidgetAircraft->windowTitle()));
  ui->dockWidgetAircraft->toggleViewAction()->setStatusTip(ui->dockWidgetAircraft->toggleViewAction()->toolTip());

  ui->dockWidgetLegend->toggleViewAction()->setIcon(QIcon(":/littlenavmap/resources/icons/legenddock.svg"));
  ui->dockWidgetLegend->toggleViewAction()->setShortcut(QKeySequence(tr("Alt+6")));
  ui->dockWidgetLegend->toggleViewAction()->setToolTip(tr("Open or show the %1 dock window").
                                                       arg(ui->dockWidgetLegend->windowTitle()));
  ui->dockWidgetLegend->toggleViewAction()->setStatusTip(ui->dockWidgetLegend->toggleViewAction()->toolTip());

  // Add dock actions to main menu
  ui->menuView->insertActions(ui->actionShowStatusbar,
                              {ui->dockWidgetSearch->toggleViewAction(),
                               ui->dockWidgetRoute->toggleViewAction(),
                               ui->dockWidgetInformation->toggleViewAction(),
                               ui->dockWidgetProfile->toggleViewAction(),
                               ui->dockWidgetAircraft->toggleViewAction(),
                               ui->dockWidgetLegend->toggleViewAction()});

  ui->menuView->insertSeparator(ui->actionShowStatusbar);

  // Add toobar actions to menu
  ui->menuView->insertActions(ui->actionShowStatusbar,
                              {ui->toolBarMain->toggleViewAction(),
                               ui->toolBarMap->toggleViewAction(),
                               ui->toolbarMapOptions->toggleViewAction(),
                               ui->toolBarRoute->toggleViewAction(),
                               ui->toolBarAirspaces->toggleViewAction(),
                               ui->toolBarView->toggleViewAction()});
  ui->menuView->insertSeparator(ui->actionShowStatusbar);

  // Add toobar actions to toolbar
  ui->toolBarView->addAction(ui->dockWidgetSearch->toggleViewAction());
  ui->toolBarView->addAction(ui->dockWidgetRoute->toggleViewAction());
  ui->toolBarView->addAction(ui->dockWidgetInformation->toggleViewAction());
  ui->toolBarView->addAction(ui->dockWidgetProfile->toggleViewAction());
  ui->toolBarView->addAction(ui->dockWidgetAircraft->toggleViewAction());
  ui->toolBarView->addAction(ui->dockWidgetLegend->toggleViewAction());

  // Create labels for the statusbar
  connectStatusLabel = new QLabel();
  connectStatusLabel->setAlignment(Qt::AlignCenter);
  connectStatusLabel->setMinimumWidth(140);
  connectStatusLabel->setText(tr("Not connected."));
  connectStatusLabel->setToolTip(tr("Simulator connection status."));
  ui->statusBar->addPermanentWidget(connectStatusLabel);

  messageLabel = new QLabel();
  messageLabel->setAlignment(Qt::AlignCenter);
  messageLabel->setMinimumWidth(150);
  ui->statusBar->addPermanentWidget(messageLabel);

  detailLabel = new QLabel();
  detailLabel->setAlignment(Qt::AlignCenter);
  detailLabel->setMinimumWidth(120);
  detailLabel->setToolTip(tr("Map detail level."));
  ui->statusBar->addPermanentWidget(detailLabel);

  renderStatusLabel = new QLabel();
  renderStatusLabel->setAlignment(Qt::AlignCenter);
  renderStatusLabel->setMinimumWidth(140);
  renderStatusLabel->setToolTip(tr("Map rendering and download status."));
  ui->statusBar->addPermanentWidget(renderStatusLabel);

  mapDistanceLabel = new QLabel();
  mapDistanceLabel->setAlignment(Qt::AlignCenter);
  mapDistanceLabel->setMinimumWidth(60);
  mapDistanceLabel->setToolTip(tr("Map view distance to ground."));
  ui->statusBar->addPermanentWidget(mapDistanceLabel);

  mapPosLabel = new QLabel();
  mapPosLabel->setAlignment(Qt::AlignCenter);
  mapPosLabel->setMinimumWidth(240);
  mapPosLabel->setToolTip(tr("Coordinates and elevation at cursor position."));
  ui->statusBar->addPermanentWidget(mapPosLabel);

  magvarLabel = new QLabel();
  magvarLabel->setAlignment(Qt::AlignCenter);
  magvarLabel->setMinimumWidth(40);
  magvarLabel->setToolTip(tr("Magnetic declination at cursor position."));
  ui->statusBar->addPermanentWidget(magvarLabel);

  timeLabel = new QLabel();
  timeLabel->setAlignment(Qt::AlignCenter);
#ifdef Q_OS_MACOS
  timeLabel->setMinimumWidth(60);
#else
  timeLabel->setMinimumWidth(55);
#endif
  timeLabel->setToolTip(tr("Day of month and UTC time."));
  ui->statusBar->addPermanentWidget(timeLabel);
}

void MainWindow::connectAllSlots()
{
  // Get "show in browser"  click
  connect(ui->textBrowserLegendNavInfo, &QTextBrowser::anchorClicked, this,
          &MainWindow::legendAnchorClicked);

  // Options dialog ===================================================================
  // Notify others of options change
  // The units need to be called before all others
  connect(optionsDialog, &OptionsDialog::optionsChanged, &Unit::optionsChanged);
  connect(optionsDialog, &OptionsDialog::optionsChanged, &NavApp::optionsChanged);

  // Need to clean cache to regenerate some text if units have changed
  connect(optionsDialog, &OptionsDialog::optionsChanged, NavApp::getProcedureQuery(), &ProcedureQuery::clearCache);

  // Reset weather context first
  connect(optionsDialog, &OptionsDialog::optionsChanged, this, &MainWindow::clearWeatherContext);

  connect(optionsDialog, &OptionsDialog::optionsChanged, this, &MainWindow::updateMapObjectsShown);
  connect(optionsDialog, &OptionsDialog::optionsChanged, this, &MainWindow::updateActionStates);
  connect(optionsDialog, &OptionsDialog::optionsChanged, this, &MainWindow::distanceChanged);
  connect(optionsDialog, &OptionsDialog::optionsChanged, weatherReporter, &WeatherReporter::optionsChanged);
  connect(optionsDialog, &OptionsDialog::optionsChanged, searchController, &SearchController::optionsChanged);
  connect(optionsDialog, &OptionsDialog::optionsChanged, map::updateUnits);
  connect(optionsDialog, &OptionsDialog::optionsChanged, routeController, &RouteController::optionsChanged);
  connect(optionsDialog, &OptionsDialog::optionsChanged, infoController, &InfoController::optionsChanged);
  connect(optionsDialog, &OptionsDialog::optionsChanged, mapWidget, &MapPaintWidget::optionsChanged);
  connect(optionsDialog, &OptionsDialog::optionsChanged, profileWidget, &ProfileWidget::optionsChanged);
  connect(optionsDialog, &OptionsDialog::optionsChanged,
          NavApp::getOnlinedataController(), &OnlinedataController::optionsChanged);
  connect(optionsDialog, &OptionsDialog::optionsChanged,
          NavApp::getElevationProvider(), &ElevationProvider::optionsChanged);
  connect(optionsDialog, &OptionsDialog::optionsChanged,
          NavApp::getAircraftPerfController(), &AircraftPerfController::optionsChanged);

  // Style handler ===================================================================
  connect(NavApp::getStyleHandler(), &StyleHandler::styleChanged, mapcolors::styleChanged);
  connect(NavApp::getStyleHandler(), &StyleHandler::styleChanged, infoController, &InfoController::optionsChanged);
  connect(NavApp::getStyleHandler(), &StyleHandler::styleChanged, routeController, &RouteController::styleChanged);
  connect(NavApp::getStyleHandler(), &StyleHandler::styleChanged, searchController, &SearchController::styleChanged);
  connect(NavApp::getStyleHandler(), &StyleHandler::styleChanged, mapWidget, &MapPaintWidget::styleChanged);
  connect(NavApp::getStyleHandler(), &StyleHandler::styleChanged, profileWidget, &ProfileWidget::styleChanged);
  connect(NavApp::getStyleHandler(), &StyleHandler::styleChanged,
          NavApp::getAircraftPerfController(), &AircraftPerfController::optionsChanged);

  // Aircraft performance signals =======================================================
  connect(NavApp::getAircraftPerfController(), &AircraftPerfController::aircraftPerformanceChanged,
          routeController, &RouteController::aircraftPerformanceChanged);

  connect(routeController, &RouteController::routeChanged,
          NavApp::getAircraftPerfController(), &AircraftPerfController::routeChanged);

  connect(routeController, &RouteController::routeAltitudeChanged,
          NavApp::getAircraftPerfController(), &AircraftPerfController::routeAltitudeChanged);

  // Route export signals =======================================================
  connect(routeExport, &RouteExport::showRect, mapWidget, &MapPaintWidget::showRect);
  connect(routeExport, &RouteExport::selectDepartureParking, routeController, &RouteController::selectDepartureParking);

  // Route controller signals =======================================================
  connect(routeController, &RouteController::showRect, mapWidget, &MapPaintWidget::showRect);
  connect(routeController, &RouteController::showPos, mapWidget, &MapPaintWidget::showPos);
  connect(routeController, &RouteController::changeMark, mapWidget, &MapWidget::changeSearchMark);
  connect(routeController, &RouteController::routeChanged, mapWidget, &MapPaintWidget::routeChanged);
  connect(routeController, &RouteController::routeAltitudeChanged, mapWidget, &MapPaintWidget::routeAltitudeChanged);
  connect(routeController, &RouteController::preRouteCalc, profileWidget, &ProfileWidget::preRouteCalc);
  connect(routeController, &RouteController::showInformation, infoController, &InfoController::showInformation);

  connect(routeController, &RouteController::showProcedures, searchController->getProcedureSearch(),
          &ProcedureSearch::showProcedures);

  // Update rubber band in map window if user hovers over profile
  connect(profileWidget, &ProfileWidget::highlightProfilePoint, mapWidget, &MapPaintWidget::changeProfileHighlight);
  connect(profileWidget, &ProfileWidget::showPos, mapWidget, &MapPaintWidget::showPos);

  connect(routeController, &RouteController::routeChanged, profileWidget, &ProfileWidget::routeChanged);
  connect(routeController, &RouteController::routeAltitudeChanged, profileWidget, &ProfileWidget::routeAltitudeChanged);
  connect(routeController, &RouteController::routeChanged, this, &MainWindow::updateActionStates);
  connect(routeController, &RouteController::routeInsert, this, &MainWindow::routeInsert);

  // Airport search ===================================================================================
  AirportSearch *airportSearch = searchController->getAirportSearch();
  connect(airportSearch, &SearchBaseTable::showRect, mapWidget, &MapPaintWidget::showRect);
  connect(airportSearch, &SearchBaseTable::showPos, mapWidget, &MapPaintWidget::showPos);
  connect(airportSearch, &SearchBaseTable::changeSearchMark, mapWidget, &MapWidget::changeSearchMark);
  connect(airportSearch, &SearchBaseTable::showInformation, infoController, &InfoController::showInformation);
  connect(airportSearch, &SearchBaseTable::showProcedures,
          searchController->getProcedureSearch(), &ProcedureSearch::showProcedures);
  connect(airportSearch, &SearchBaseTable::routeSetDeparture, routeController, &RouteController::routeSetDeparture);
  connect(airportSearch, &SearchBaseTable::routeSetDestination, routeController, &RouteController::routeSetDestination);
  connect(airportSearch, &SearchBaseTable::routeAdd, routeController, &RouteController::routeAdd);
  connect(airportSearch, &SearchBaseTable::selectionChanged, this, &MainWindow::searchSelectionChanged);

  // Nav search ===================================================================================
  NavSearch *navSearch = searchController->getNavSearch();
  connect(navSearch, &SearchBaseTable::showPos, mapWidget, &MapPaintWidget::showPos);
  connect(navSearch, &SearchBaseTable::changeSearchMark, mapWidget, &MapWidget::changeSearchMark);
  connect(navSearch, &SearchBaseTable::showInformation, infoController, &InfoController::showInformation);
  connect(navSearch, &SearchBaseTable::selectionChanged, this, &MainWindow::searchSelectionChanged);
  connect(navSearch, &SearchBaseTable::routeAdd, routeController, &RouteController::routeAdd);

  // Userdata search ===================================================================================
  UserdataSearch *userSearch = searchController->getUserdataSearch();
  connect(userSearch, &SearchBaseTable::showPos, mapWidget, &MapPaintWidget::showPos);
  connect(userSearch, &SearchBaseTable::changeSearchMark, mapWidget, &MapWidget::changeSearchMark);
  connect(userSearch, &SearchBaseTable::showInformation, infoController, &InfoController::showInformation);
  connect(userSearch, &SearchBaseTable::selectionChanged, this, &MainWindow::searchSelectionChanged);
  connect(userSearch, &SearchBaseTable::routeAdd, routeController, &RouteController::routeAdd);

  // User data ===================================================================================
  UserdataController *userdataController = NavApp::getUserdataController();
  connect(ui->actionUserdataClearDatabase, &QAction::triggered, userdataController, &UserdataController::clearDatabase);
  connect(ui->actionUserdataShowSearch, &QAction::triggered, userdataController, &UserdataController::showSearch);

  // Import ================
  connect(ui->actionUserdataImportCSV, &QAction::triggered, userdataController, &UserdataController::importCsv);
  connect(ui->actionUserdataImportGarminGTN, &QAction::triggered, userdataController,
          &UserdataController::importGarmin);
  connect(ui->actionUserdataImportUserfixDat, &QAction::triggered, userdataController,
          &UserdataController::importXplaneUserFixDat);

  // Export ================
  connect(ui->actionUserdataExportCSV, &QAction::triggered, userdataController, &UserdataController::exportCsv);
  connect(ui->actionUserdataExportGarminGTN, &QAction::triggered, userdataController,
          &UserdataController::exportGarmin);
  connect(ui->actionUserdataExportUserfixDat, &QAction::triggered, userdataController,
          &UserdataController::exportXplaneUserFixDat);
  connect(ui->actionUserdataExportXmlBgl, &QAction::triggered, userdataController,
          &UserdataController::exportBglXml);

  connect(userdataController, &UserdataController::userdataChanged, infoController,
          &InfoController::updateAllInformation);
  connect(userdataController, &UserdataController::userdataChanged, this, &MainWindow::updateMapObjectsShown);
  connect(userdataController, &UserdataController::refreshUserdataSearch, userSearch, &UserdataSearch::refreshData);

  connect(mapWidget, &MapWidget::aircraftTakeoff, userdataController, &UserdataController::aircraftTakeoff);
  connect(mapWidget, &MapWidget::aircraftLanding, userdataController, &UserdataController::aircraftLanding);

  // Online search ===================================================================================
  OnlineClientSearch *clientSearch = searchController->getOnlineClientSearch();
  OnlineCenterSearch *centerSearch = searchController->getOnlineCenterSearch();
  OnlineServerSearch *serverSearch = searchController->getOnlineServerSearch();
  OnlinedataController *onlinedataController = NavApp::getOnlinedataController();

  // Online client search ===================================================================================
  connect(clientSearch, &SearchBaseTable::showRect, mapWidget, &MapPaintWidget::showRect);
  connect(clientSearch, &SearchBaseTable::showPos, mapWidget, &MapPaintWidget::showPos);
  connect(clientSearch, &SearchBaseTable::changeSearchMark, mapWidget, &MapWidget::changeSearchMark);
  connect(clientSearch, &SearchBaseTable::showInformation, infoController, &InfoController::showInformation);
  connect(clientSearch, &SearchBaseTable::selectionChanged, this, &MainWindow::searchSelectionChanged);

  // Online center search ===================================================================================
  connect(centerSearch, &SearchBaseTable::showRect, mapWidget, &MapPaintWidget::showRect);
  connect(centerSearch, &SearchBaseTable::showPos, mapWidget, &MapPaintWidget::showPos);
  connect(centerSearch, &SearchBaseTable::changeSearchMark, mapWidget, &MapWidget::changeSearchMark);
  connect(centerSearch, &SearchBaseTable::showInformation, infoController, &InfoController::showInformation);
  connect(centerSearch, &SearchBaseTable::selectionChanged, this, &MainWindow::searchSelectionChanged);

  // Remove/add buttons and tabs
  connect(onlinedataController, &OnlinedataController::onlineNetworkChanged,
          this, &MainWindow::updateOnlineActionStates);

  // Update search
  connect(onlinedataController, &OnlinedataController::onlineClientAndAtcUpdated,
          clientSearch, &OnlineClientSearch::refreshData);
  connect(onlinedataController, &OnlinedataController::onlineClientAndAtcUpdated,
          centerSearch, &OnlineCenterSearch::refreshData);
  connect(onlinedataController, &OnlinedataController::onlineServersUpdated,
          serverSearch, &OnlineServerSearch::refreshData);

  // Clear cache and update map widget
  connect(onlinedataController, &OnlinedataController::onlineClientAndAtcUpdated,
          NavApp::getAirspaceQueryOnline(), &AirspaceQuery::clearCache);
  connect(onlinedataController, &OnlinedataController::onlineClientAndAtcUpdated,
          mapWidget, &MapPaintWidget::onlineClientAndAtcUpdated);
  connect(onlinedataController, &OnlinedataController::onlineNetworkChanged,
          mapWidget, &MapPaintWidget::onlineNetworkChanged);

  // Update info
  connect(onlinedataController, &OnlinedataController::onlineClientAndAtcUpdated,
          infoController, &InfoController::onlineClientAndAtcUpdated);
  connect(onlinedataController, &OnlinedataController::onlineNetworkChanged,
          infoController, &InfoController::onlineNetworkChanged);

  connect(onlinedataController, &OnlinedataController::onlineNetworkChanged,
          airspaceHandler, &AirspaceToolBarHandler::updateButtonsAndActions);
  connect(onlinedataController, &OnlinedataController::onlineClientAndAtcUpdated,
          airspaceHandler, &AirspaceToolBarHandler::updateButtonsAndActions);

  connect(clientSearch, &SearchBaseTable::selectionChanged, this, &MainWindow::searchSelectionChanged);
  connect(centerSearch, &SearchBaseTable::selectionChanged, this, &MainWindow::searchSelectionChanged);

  // Approach controller ===================================================================
  ProcedureSearch *procedureSearch = searchController->getProcedureSearch();
  connect(procedureSearch, &ProcedureSearch::procedureLegSelected, this, &MainWindow::procedureLegSelected);
  connect(procedureSearch, &ProcedureSearch::procedureSelected, this, &MainWindow::procedureSelected);
  connect(procedureSearch, &ProcedureSearch::showRect, mapWidget, &MapPaintWidget::showRect);
  connect(procedureSearch, &ProcedureSearch::showPos, mapWidget, &MapPaintWidget::showPos);
  connect(procedureSearch, &ProcedureSearch::routeInsertProcedure, routeController,
          &RouteController::routeAttachProcedure);
  connect(procedureSearch, &ProcedureSearch::showInformation, infoController, &InfoController::showInformation);

  connect(ui->actionResetLayout, &QAction::triggered, this, &MainWindow::resetWindowLayout);

  connect(infoController, &InfoController::showPos, mapWidget, &MapPaintWidget::showPos);
  connect(infoController, &InfoController::showRect, mapWidget, &MapPaintWidget::showRect);

  // Use this event to show scenery library dialog on first start after main windows is shown
  connect(this, &MainWindow::windowShown, this, &MainWindow::mainWindowShown, Qt::QueuedConnection);

  connect(ui->actionShowStatusbar, &QAction::toggled, ui->statusBar, &QStatusBar::setVisible);

  connect(ui->actionReloadScenery, &QAction::triggered, NavApp::getDatabaseManager(), &DatabaseManager::run);
  connect(ui->actionReloadSceneryCopyAirspaces, &QAction::triggered,
          NavApp::getDatabaseManager(), &DatabaseManager::copyAirspaces);
  connect(ui->actionDatabaseFiles, &QAction::triggered, this, &MainWindow::showDatabaseFiles);

  connect(ui->actionOptions, &QAction::triggered, this, &MainWindow::options);
  connect(ui->actionResetMessages, &QAction::triggered, this, &MainWindow::resetMessages);

  // Windows menu ============================================================
  connect(ui->actionShowFloatingWindows, &QAction::triggered, this, &MainWindow::raiseFloatingWindows);

  // File menu ============================================================
  connect(ui->actionExit, &QAction::triggered, this, &MainWindow::close);

  // Flight plan file actions ============================================================
  connect(ui->actionRouteCenter, &QAction::triggered, this, &MainWindow::routeCenter);
  connect(ui->actionRouteNew, &QAction::triggered, this, &MainWindow::routeNew);
  connect(ui->actionRouteNewFromString, &QAction::triggered, this, &MainWindow::routeNewFromString);
  connect(ui->actionRouteOpen, &QAction::triggered, this, &MainWindow::routeOpen);
  connect(ui->actionRouteAppend, &QAction::triggered, this, &MainWindow::routeAppend);
  connect(ui->actionRouteTableAppend, &QAction::triggered, this, &MainWindow::routeAppend);
  connect(ui->actionRouteSave, &QAction::triggered, this, &MainWindow::routeSave);
  connect(ui->actionRouteSaveAs, &QAction::triggered, this, &MainWindow::routeSaveAsPln);
  connect(ui->actionRouteSaveAsFlp, &QAction::triggered, this, &MainWindow::routeSaveAsFlp);
  connect(ui->actionRouteSaveAsFlightGear, &QAction::triggered, this, &MainWindow::routeSaveAsFlightGear);
  connect(ui->actionRouteSaveAsFms3, &QAction::triggered, this, &MainWindow::routeSaveAsFms3);
  connect(ui->actionRouteSaveAsFms11, &QAction::triggered, this, &MainWindow::routeSaveAsFms11);
  connect(ui->actionRouteSaveAsClean, &QAction::triggered, this, &MainWindow::routeExportClean);

  // Flight plan export actions =====================================================================
  connect(ui->actionRouteSaveAsGfp, &QAction::triggered, routeExport, &RouteExport::routeExportGfp);
  connect(ui->actionRouteSaveAsTxt, &QAction::triggered, routeExport, &RouteExport::routeExportTxt);
  connect(ui->actionRouteSaveAsRte, &QAction::triggered, routeExport, &RouteExport::routeExportRte);
  connect(ui->actionRouteSaveAsFpr, &QAction::triggered, routeExport, &RouteExport::routeExportFpr);
  connect(ui->actionRouteSaveAsFpl, &QAction::triggered, routeExport, &RouteExport::routeExportFpl);
  connect(ui->actionRouteSaveAsCorteIn, &QAction::triggered, routeExport, &RouteExport::routeExportCorteIn);
  connect(ui->actionRouteSaveAsGpx, &QAction::triggered, routeExport, &RouteExport::routeExportGpx);
  connect(ui->actionRouteShowSkyVector, &QAction::triggered, this, &MainWindow::openInSkyVector);

  connect(ui->actionRouteSaveAsRxpGns, &QAction::triggered, routeExport, &RouteExport::routeExportRxpGns);
  connect(ui->actionRouteSaveAsRxpGtn, &QAction::triggered, routeExport, &RouteExport::routeExportRxpGtn);

  connect(ui->actionRouteSaveAsIFly, &QAction::triggered, routeExport, &RouteExport::routeExportFltplan);
  connect(ui->actionRouteSaveAsXFmc, &QAction::triggered, routeExport, &RouteExport::routeExportXFmc);
  connect(ui->actionRouteSaveAsUfmc, &QAction::triggered, routeExport, &RouteExport::routeExportUFmc);
  connect(ui->actionRouteSaveAsProSim, &QAction::triggered, routeExport, &RouteExport::routeExportProSim);
  connect(ui->actionRouteSaveAsBbsAirbus, &QAction::triggered, routeExport, &RouteExport::routeExportBbs);
  connect(ui->actionRouteSaveAsLeveldRte, &QAction::triggered, routeExport, &RouteExport::routeExportLeveldRte);
  connect(ui->actionRouteSaveAsFeelthereFpl, &QAction::triggered, routeExport, &RouteExport::routeExportFeelthereFpl);
  connect(ui->actionRouteSaveAsEfbr, &QAction::triggered, routeExport, &RouteExport::routeExportEfbr);
  connect(ui->actionRouteSaveAsQwRte, &QAction::triggered, routeExport, &RouteExport::routeExportQwRte);
  connect(ui->actionRouteSaveAsMdx, &QAction::triggered, routeExport, &RouteExport::routeExportMdx);

  // Online export options
  connect(ui->actionRouteSaveAsVfp, &QAction::triggered, routeExport, &RouteExport::routeExportVfp);
  connect(ui->actionRouteSaveAsIvap, &QAction::triggered, routeExport, &RouteExport::routeExportIvap);

  connect(routeFileHistory, &FileHistoryHandler::fileSelected, this, &MainWindow::routeOpenRecent);

  connect(ui->actionPrintMap, &QAction::triggered, printSupport, &PrintSupport::printMap);
  connect(ui->actionPrintFlightplan, &QAction::triggered, printSupport, &PrintSupport::printFlightplan);
  connect(ui->actionSaveMapAsImage, &QAction::triggered, this, &MainWindow::mapSaveImage);
  connect(ui->actionSaveMapAsImageAviTab, &QAction::triggered, this, &MainWindow::mapSaveImageAviTab);
  connect(ui->actionMapCopyClipboard, &QAction::triggered, this, &MainWindow::mapCopyToClipboard);

  // KML actions
  connect(ui->actionLoadKml, &QAction::triggered, this, &MainWindow::kmlOpen);
  connect(ui->actionClearKml, &QAction::triggered, this, &MainWindow::kmlClear);

  connect(kmlFileHistory, &FileHistoryHandler::fileSelected, this, &MainWindow::kmlOpenRecent);

  // Flight plan calculation
  connect(ui->actionRouteCalcDirect, &QAction::triggered, routeController, &RouteController::calculateDirect);

  connect(ui->actionRouteCalcRadionav, &QAction::triggered,
          routeController, static_cast<void (RouteController::*)()>(&RouteController::calculateRadionav));
  connect(ui->actionRouteCalcHighAlt, &QAction::triggered,
          routeController, static_cast<void (RouteController::*)()>(&RouteController::calculateHighAlt));
  connect(ui->actionRouteCalcLowAlt, &QAction::triggered,
          routeController, static_cast<void (RouteController::*)()>(&RouteController::calculateLowAlt));
  connect(ui->actionRouteCalcSetAlt, &QAction::triggered,
          routeController, static_cast<void (RouteController::*)()>(&RouteController::calculateSetAlt));
  connect(ui->actionRouteReverse, &QAction::triggered, routeController, &RouteController::reverseRoute);

  connect(ui->actionRouteCopyString, &QAction::triggered, routeController, &RouteController::routeStringToClipboard);

  connect(ui->actionRouteAdjustAltitude, &QAction::triggered, routeController,
          &RouteController::adjustFlightplanAltitude);

  // Help menu
  connect(ui->actionHelpContents, &QAction::triggered, this, &MainWindow::showOnlineHelp);
  connect(ui->actionHelpTutorials, &QAction::triggered, this, &MainWindow::showOnlineTutorials);
  connect(ui->actionHelpContentsOffline, &QAction::triggered, this, &MainWindow::showOfflineHelp);
  connect(ui->actionHelpAbout, &QAction::triggered, helpHandler, &atools::gui::HelpHandler::about);
  connect(ui->actionHelpAboutQt, &QAction::triggered, helpHandler, &atools::gui::HelpHandler::aboutQt);
  connect(ui->actionHelpCheckUpdates, &QAction::triggered, this, &MainWindow::checkForUpdates);
  connect(ui->actionHelpDonate, &QAction::triggered, this, &MainWindow::showDonationPage);
  connect(ui->actionHelpFaq, &QAction::triggered, this, &MainWindow::showFaqPage);

  // Map widget related connections
  connect(mapWidget, &MapWidget::showInSearch, searchController, &SearchController::showInSearch);
  // Connect the map widget to the position label.
  connect(mapWidget, &MapPaintWidget::distanceChanged, this, &MainWindow::distanceChanged);
  connect(mapWidget, &MapPaintWidget::renderStatusChanged, this, &MainWindow::renderStatusChanged);
  connect(mapWidget, &MapPaintWidget::updateActionStates, this, &MainWindow::updateActionStates);
  connect(mapWidget, &MapWidget::showInformation, infoController, &InfoController::showInformation);
  connect(mapWidget, &MapWidget::showApproaches,
          searchController->getProcedureSearch(), &ProcedureSearch::showProcedures);
  connect(mapWidget, &MapPaintWidget::shownMapFeaturesChanged,
          routeController, &RouteController::shownMapFeaturesChanged);
  connect(mapWidget, &MapWidget::addUserpointFromMap,
          NavApp::getUserdataController(), &UserdataController::addUserpointFromMap);
  connect(mapWidget, &MapWidget::editUserpointFromMap,
          NavApp::getUserdataController(), &UserdataController::editUserpointFromMap);
  connect(mapWidget, &MapWidget::deleteUserpointFromMap,
          NavApp::getUserdataController(), &UserdataController::deleteUserpointFromMap);
  connect(mapWidget, &MapWidget::moveUserpointFromMap,
          NavApp::getUserdataController(), &UserdataController::moveUserpointFromMap);

  // Connect toolbar combo boxes
  void (QComboBox::*indexChangedPtr)(int) = &QComboBox::currentIndexChanged;
  connect(mapProjectionComboBox, indexChangedPtr, this, &MainWindow::changeMapProjection);
  connect(mapThemeComboBox, indexChangedPtr, this, &MainWindow::changeMapTheme);

  // Let projection menus update combo boxes
  connect(ui->actionMapProjectionMercator, &QAction::triggered, [this](bool checked)
  {
    if(checked)
      mapProjectionComboBox->setCurrentIndex(0);
  });

  connect(ui->actionMapProjectionSpherical, &QAction::triggered, [this](bool checked)
  {
    if(checked)
      mapProjectionComboBox->setCurrentIndex(1);
  });

  // Let theme menus update combo boxes
  connect(ui->actionMapThemeOpenStreetMap, &QAction::triggered, this, &MainWindow::themeMenuTriggered);
  connect(ui->actionMapThemeOpenStreetMapRoads, &QAction::triggered, this, &MainWindow::themeMenuTriggered);
  connect(ui->actionMapThemeOpenTopoMap, &QAction::triggered, this, &MainWindow::themeMenuTriggered);
  connect(ui->actionMapThemeStamenTerrain, &QAction::triggered, this, &MainWindow::themeMenuTriggered);
  connect(ui->actionMapThemeCartoLight, &QAction::triggered, this, &MainWindow::themeMenuTriggered);
  connect(ui->actionMapThemeCartoDark, &QAction::triggered, this, &MainWindow::themeMenuTriggered);
  connect(ui->actionMapThemeSimple, &QAction::triggered, this, &MainWindow::themeMenuTriggered);
  connect(ui->actionMapThemePlain, &QAction::triggered, this, &MainWindow::themeMenuTriggered);
  connect(ui->actionMapThemeAtlas, &QAction::triggered, this, &MainWindow::themeMenuTriggered);

  for(QAction *action : customMapThemeMenuActions)
    connect(action, &QAction::triggered, this, &MainWindow::themeMenuTriggered);

  // Map object/feature display
  connect(ui->actionMapShowMinimumAltitude, &QAction::toggled, this, &MainWindow::updateMapObjectsShown);
  connect(ui->actionMapShowAirportWeather, &QAction::toggled, this, &MainWindow::updateMapObjectsShown);
  connect(ui->actionMapShowCities, &QAction::toggled, this, &MainWindow::updateMapObjectsShown);
  connect(ui->actionMapShowGrid, &QAction::toggled, this, &MainWindow::updateMapObjectsShown);
  connect(ui->actionMapShowHillshading, &QAction::toggled, this, &MainWindow::updateMapObjectsShown);
  connect(ui->actionMapShowAirports, &QAction::toggled, this, &MainWindow::updateMapObjectsShown);
  connect(ui->actionMapShowSoftAirports, &QAction::toggled, this, &MainWindow::updateMapObjectsShown);
  connect(ui->actionMapShowEmptyAirports, &QAction::toggled, this, &MainWindow::updateMapObjectsShown);
  connect(ui->actionMapShowAddonAirports, &QAction::toggled, this, &MainWindow::updateMapObjectsShown);
  connect(ui->actionMapShowVor, &QAction::toggled, this, &MainWindow::updateMapObjectsShown);
  connect(ui->actionMapShowNdb, &QAction::toggled, this, &MainWindow::updateMapObjectsShown);
  connect(ui->actionMapShowWp, &QAction::toggled, this, &MainWindow::updateMapObjectsShown);
  connect(ui->actionMapShowIls, &QAction::toggled, this, &MainWindow::updateMapObjectsShown);
  connect(ui->actionMapShowVictorAirways, &QAction::toggled, this, &MainWindow::updateMapObjectsShown);
  connect(ui->actionMapShowJetAirways, &QAction::toggled, this, &MainWindow::updateMapObjectsShown);
  connect(ui->actionMapShowRoute, &QAction::toggled, this, &MainWindow::updateMapObjectsShown);
  connect(ui->actionMapHideRangeRings, &QAction::triggered, mapWidget, &MapWidget::clearRangeRingsAndDistanceMarkers);

  connect(ui->actionMapShowAirportWeather, &QAction::toggled, infoController, &InfoController::updateAirportWeather);

  // Clear selection and highlights
  connect(ui->actionMapClearAllHighlights, &QAction::triggered, routeController, &RouteController::clearSelection);
  connect(ui->actionMapClearAllHighlights, &QAction::triggered, searchController, &SearchController::clearSelection);
  connect(ui->actionMapClearAllHighlights, &QAction::triggered, mapWidget, &MapPaintWidget::clearSearchHighlights);
  connect(ui->actionMapClearAllHighlights, &QAction::triggered, mapWidget, &MapPaintWidget::clearAirspaceHighlights);
  connect(ui->actionMapClearAllHighlights, &QAction::triggered, this, &MainWindow::updateHighlightActionStates);

  connect(ui->actionInfoApproachShowMissedAppr, &QAction::toggled, this, &MainWindow::updateMapObjectsShown);

  connect(ui->actionMapShowCompassRose, &QAction::toggled, this, &MainWindow::updateMapObjectsShown);
  connect(ui->actionMapShowAircraft, &QAction::toggled, this, &MainWindow::updateMapObjectsShown);
  connect(ui->actionMapShowAircraftAi, &QAction::toggled, this, &MainWindow::updateMapObjectsShown);
  connect(ui->actionMapShowAircraftAiBoat, &QAction::toggled, this, &MainWindow::updateMapObjectsShown);
  connect(ui->actionMapShowAircraftTrack, &QAction::toggled, this, &MainWindow::updateMapObjectsShown);
  connect(ui->actionShowAirspaces, &QAction::toggled, this, &MainWindow::updateMapObjectsShown);
  connect(ui->actionShowAirspacesOnline, &QAction::toggled, this, &MainWindow::updateMapObjectsShown);
  connect(ui->actionMapResetSettings, &QAction::triggered, this, &MainWindow::resetMapObjectsShown);

  connect(ui->actionMapShowAircraft, &QAction::toggled, profileWidget, &ProfileWidget::updateProfileShowFeatures);
  connect(ui->actionMapShowAircraftTrack, &QAction::toggled, profileWidget, &ProfileWidget::updateProfileShowFeatures);

  // Weather source
  connect(ui->actionMapShowWeatherSimulator, &QAction::toggled, this, &MainWindow::updateMapObjectsShown);
  connect(ui->actionMapShowWeatherActiveSky, &QAction::toggled, this, &MainWindow::updateMapObjectsShown);
  connect(ui->actionMapShowWeatherNoaa, &QAction::toggled, this, &MainWindow::updateMapObjectsShown);
  connect(ui->actionMapShowWeatherVatsim, &QAction::toggled, this, &MainWindow::updateMapObjectsShown);
  connect(ui->actionMapShowWeatherIvao, &QAction::toggled, this, &MainWindow::updateMapObjectsShown);

  // Update map weather source hightlights
  connect(ui->actionMapShowWeatherSimulator, &QAction::toggled, infoController, &InfoController::updateAirportWeather);
  connect(ui->actionMapShowWeatherActiveSky, &QAction::toggled, infoController, &InfoController::updateAirportWeather);
  connect(ui->actionMapShowWeatherNoaa, &QAction::toggled, infoController, &InfoController::updateAirportWeather);
  connect(ui->actionMapShowWeatherVatsim, &QAction::toggled, infoController, &InfoController::updateAirportWeather);
  connect(ui->actionMapShowWeatherIvao, &QAction::toggled, infoController, &InfoController::updateAirportWeather);

  // Sun shading
  connect(ui->actionMapShowSunShading, &QAction::toggled, this, &MainWindow::updateMapObjectsShown);
  connect(ui->actionMapShowSunShadingSimulatorTime, &QAction::triggered, this, &MainWindow::sunShadingTimeChanged);
  connect(ui->actionMapShowSunShadingRealTime, &QAction::triggered, this, &MainWindow::sunShadingTimeChanged);
  connect(ui->actionMapShowSunShadingUserTime, &QAction::triggered, this, &MainWindow::sunShadingTimeChanged);
  connect(ui->actionMapShowSunShadingSetTime, &QAction::triggered, this, &MainWindow::sunShadingTimeSet);

  // Update information after updateMapObjectsShownr updated the flags ============================
  connect(ui->actionMapShowAircraft, &QAction::toggled, infoController, &InfoController::updateAllInformation);
  connect(ui->actionMapShowAircraftAi, &QAction::toggled, infoController, &InfoController::updateAllInformation);
  connect(ui->actionMapShowAircraftAiBoat, &QAction::toggled, infoController, &InfoController::updateAllInformation);

  // Order is important here. First let the mapwidget delete the track then notify the profile
  connect(ui->actionMapDeleteAircraftTrack, &QAction::triggered, this, &MainWindow::deleteAircraftTrack);

  connect(ui->actionMapShowMark, &QAction::triggered, mapWidget, &MapWidget::showSearchMark);
  connect(ui->actionMapShowHome, &QAction::triggered, mapWidget, &MapWidget::showHome);
  connect(ui->actionMapAircraftCenter, &QAction::toggled, mapWidget, &MapPaintWidget::showAircraft);

  // Update jump back
  connect(ui->actionMapAircraftCenter, &QAction::toggled, mapWidget, &MapPaintWidget::jumpBackToAircraftCancel);

  connect(ui->actionMapBack, &QAction::triggered, mapWidget, &MapWidget::historyBack);
  connect(ui->actionMapNext, &QAction::triggered, mapWidget, &MapWidget::historyNext);
  connect(ui->actionWorkOffline, &QAction::toggled, mapWidget, &MapWidget::workOffline);

  connect(ui->actionMapMoreDetails, &QAction::triggered, mapWidget, &MapWidget::increaseMapDetail);
  connect(ui->actionMapLessDetails, &QAction::triggered, mapWidget, &MapWidget::decreaseMapDetail);
  connect(ui->actionMapDefaultDetails, &QAction::triggered, mapWidget, &MapWidget::defaultMapDetail);

  connect(ui->actionMapSetHome, &QAction::triggered, mapWidget, &MapWidget::changeHome);

  connect(mapWidget->getHistory(), &MapPosHistory::historyChanged, this, &MainWindow::updateMapHistoryActions);

  connect(routeController, &RouteController::routeSelectionChanged, this, &MainWindow::routeSelectionChanged);

  connect(ui->actionRouteSelectParking, &QAction::triggered, routeController, &RouteController::selectDepartureParking);

  // Route editing
  connect(mapWidget, &MapWidget::routeSetStart, routeController, &RouteController::routeSetDeparture);
  connect(mapWidget, &MapWidget::routeSetParkingStart, routeController, &RouteController::routeSetParking);
  connect(mapWidget, &MapWidget::routeSetHelipadStart, routeController, &RouteController::routeSetHelipad);
  connect(mapWidget, &MapWidget::routeSetDest, routeController, &RouteController::routeSetDestination);
  connect(mapWidget, &MapWidget::routeAdd, routeController, &RouteController::routeAdd);
  connect(mapWidget, &MapWidget::routeReplace, routeController, &RouteController::routeReplace);

  // Messages about database query result status
  connect(mapWidget, &MapPaintWidget::resultTruncated, this, &MainWindow::resultTruncated);

  connect(NavApp::getDatabaseManager(), &DatabaseManager::preDatabaseLoad, this, &MainWindow::preDatabaseLoad);
  connect(NavApp::getDatabaseManager(), &DatabaseManager::postDatabaseLoad, this, &MainWindow::postDatabaseLoad);

  // Not needed. All properties removed from legend since they are not persistent
  // connect(legendWidget, &Marble::LegendWidget::propertyValueChanged,
  // mapWidget, &MapWidget::setPropertyValue);

  connect(ui->actionAboutMarble, &QAction::triggered, marbleAbout, &Marble::MarbleAboutDialog::exec);

  ConnectClient *connectClient = NavApp::getConnectClient();
  connect(ui->actionConnectSimulator, &QAction::triggered, connectClient, &ConnectClient::connectToServerDialog);

  // Deliver first to route controller to update active leg and distances
  connect(connectClient, &ConnectClient::dataPacketReceived, routeController, &RouteController::simDataChanged);

  connect(connectClient, &ConnectClient::dataPacketReceived, mapWidget, &MapWidget::simDataChanged);
  connect(connectClient, &ConnectClient::dataPacketReceived, profileWidget, &ProfileWidget::simDataChanged);
  connect(connectClient, &ConnectClient::dataPacketReceived, infoController, &InfoController::simDataChanged);
  connect(connectClient, &ConnectClient::dataPacketReceived,
          NavApp::getAircraftPerfController(), &AircraftPerfController::simDataChanged);

  connect(connectClient, &ConnectClient::disconnectedFromSimulator, routeController,
          &RouteController::disconnectedFromSimulator);

  connect(connectClient, &ConnectClient::disconnectedFromSimulator, this, &MainWindow::sunShadingTimeChanged);

  // Map widget needs to clear track first
  connect(connectClient, &ConnectClient::connectedToSimulator, mapWidget, &MapPaintWidget::connectedToSimulator);
  connect(connectClient, &ConnectClient::disconnectedFromSimulator, mapWidget,
          &MapPaintWidget::disconnectedFromSimulator);

  connect(connectClient, &ConnectClient::connectedToSimulator, this, &MainWindow::updateActionStates);
  connect(connectClient, &ConnectClient::disconnectedFromSimulator, this, &MainWindow::updateActionStates);

  connect(connectClient, &ConnectClient::connectedToSimulator, infoController, &InfoController::connectedToSimulator);
  connect(connectClient, &ConnectClient::disconnectedFromSimulator, infoController,
          &InfoController::disconnectedFromSimulator);

  connect(connectClient, &ConnectClient::connectedToSimulator, profileWidget, &ProfileWidget::connectedToSimulator);
  connect(connectClient, &ConnectClient::disconnectedFromSimulator, profileWidget,
          &ProfileWidget::disconnectedFromSimulator);
  connect(connectClient, &ConnectClient::aiFetchOptionsChanged, this, &MainWindow::updateActionStates);

  connect(mapWidget, &MapPaintWidget::aircraftTrackPruned, profileWidget, &ProfileWidget::aircraftTrackPruned);

  connect(weatherReporter, &WeatherReporter::weatherUpdated, mapWidget, &MapWidget::updateTooltip);
  connect(weatherReporter, &WeatherReporter::weatherUpdated, infoController, &InfoController::updateAirportWeather);
  connect(weatherReporter, &WeatherReporter::weatherUpdated, mapWidget, &MapPaintWidget::weatherUpdated);

  connect(connectClient, &ConnectClient::weatherUpdated, mapWidget, &MapPaintWidget::weatherUpdated);
  connect(connectClient, &ConnectClient::weatherUpdated, mapWidget, &MapWidget::updateTooltip);
  connect(connectClient, &ConnectClient::weatherUpdated, infoController, &InfoController::updateAirportWeather);

  connect(ui->actionHelpNavmapLegend, &QAction::triggered, this, &MainWindow::showNavmapLegend);
  connect(ui->actionHelpMapLegend, &QAction::triggered, this, &MainWindow::showMapLegend);

  connect(&weatherUpdateTimer, &QTimer::timeout, this, &MainWindow::weatherUpdateTimeout);

  connect(airspaceHandler, &AirspaceToolBarHandler::updateAirspaceTypes, this, &MainWindow::updateAirspaceTypes);

  // Shortcut menu
  connect(ui->actionShortcutMap, &QAction::triggered,
          this, &MainWindow::actionShortcutMapTriggered);
  connect(ui->actionShortcutProfile, &QAction::triggered,
          this, &MainWindow::actionShortcutProfileTriggered);
  connect(ui->actionShortcutAirportSearch, &QAction::triggered,
          this, &MainWindow::actionShortcutAirportSearchTriggered);
  connect(ui->actionShortcutNavaidSearch, &QAction::triggered,
          this, &MainWindow::actionShortcutNavaidSearchTriggered);
  connect(ui->actionShortcutUserpointSearch, &QAction::triggered,
          this, &MainWindow::actionShortcutUserpointSearchTriggered);
  connect(ui->actionShortcutFlightPlan, &QAction::triggered,
          this, &MainWindow::actionShortcutFlightPlanTriggered);
  connect(ui->actionShortcutAircraftPerformance, &QAction::triggered,
          this, &MainWindow::actionShortcutAircraftPerformanceTriggered);
  connect(ui->actionShortcutAirportInformation, &QAction::triggered,
          this, &MainWindow::actionShortcutAirportInformationTriggered);
  connect(ui->actionShortcutAirportWeather, &QAction::triggered,
          this, &MainWindow::actionShortcutAirportWeatherTriggered);
  connect(ui->actionShortcutNavaidInformation, &QAction::triggered,
          this, &MainWindow::actionShortcutNavaidInformationTriggered);
  connect(ui->actionShortcutAircraftProgress, &QAction::triggered,
          this, &MainWindow::actionShortcutAircraftProgressTriggered);
}

void MainWindow::actionShortcutMapTriggered()
{
  qDebug() << Q_FUNC_INFO;
  if(OptionData::instance().getFlags2() & opts::MAP_ALLOW_UNDOCK)
  {
    ui->dockWidgetMap->show();
    ui->dockWidgetMap->activateWindow();
    raiseFloatingWindow(ui->dockWidgetMap);
  }
  mapWidget->activateWindow();
  mapWidget->setFocus();
}

void MainWindow::actionShortcutProfileTriggered()
{
  qDebug() << Q_FUNC_INFO;
  ui->dockWidgetProfile->show();
  ui->dockWidgetProfile->activateWindow();
  ui->dockWidgetProfile->raise();
  profileWidget->activateWindow();
  profileWidget->setFocus();
}

void MainWindow::actionShortcutAirportSearchTriggered()
{
  qDebug() << Q_FUNC_INFO;
  ui->dockWidgetSearch->show();
  ui->dockWidgetSearch->activateWindow();
  ui->dockWidgetSearch->raise();
  ui->tabWidgetSearch->setCurrentIndex(si::SEARCH_AIRPORT);
  ui->lineEditAirportIcaoSearch->setFocus();
}

void MainWindow::actionShortcutNavaidSearchTriggered()
{
  qDebug() << Q_FUNC_INFO;
  ui->dockWidgetSearch->show();
  ui->dockWidgetSearch->activateWindow();
  ui->dockWidgetSearch->raise();
  ui->tabWidgetSearch->setCurrentIndex(si::SEARCH_NAV);
  ui->lineEditNavIcaoSearch->setFocus();
}

void MainWindow::actionShortcutUserpointSearchTriggered()
{
  qDebug() << Q_FUNC_INFO;
  ui->dockWidgetSearch->show();
  ui->dockWidgetSearch->activateWindow();
  ui->dockWidgetSearch->raise();
  ui->tabWidgetSearch->setCurrentIndex(si::SEARCH_USER);
  ui->lineEditUserdataIdent->setFocus();
}

void MainWindow::actionShortcutFlightPlanTriggered()
{
  qDebug() << Q_FUNC_INFO;
  ui->dockWidgetRoute->show();
  ui->dockWidgetRoute->activateWindow();
  ui->dockWidgetRoute->raise();
  ui->tabWidgetRoute->setCurrentIndex(rc::ROUTE);
  ui->tableViewRoute->setFocus();
}

void MainWindow::actionShortcutAircraftPerformanceTriggered()
{
  qDebug() << Q_FUNC_INFO;
  ui->dockWidgetRoute->show();
  ui->dockWidgetRoute->activateWindow();
  ui->dockWidgetRoute->raise();
  ui->tabWidgetRoute->setCurrentIndex(rc::AIRCRAFT);
  ui->textBrowserAircraftPerformanceReport->setFocus();
}

void MainWindow::actionShortcutAirportInformationTriggered()
{
  qDebug() << Q_FUNC_INFO;
  ui->dockWidgetInformation->show();
  ui->dockWidgetInformation->activateWindow();
  ui->dockWidgetInformation->raise();
  ui->tabWidgetInformation->setCurrentIndex(ic::INFO_AIRPORT);
  ui->textBrowserAirportInfo->setFocus();
}

void MainWindow::actionShortcutAirportWeatherTriggered()
{
  qDebug() << Q_FUNC_INFO;
  ui->dockWidgetInformation->show();
  ui->dockWidgetInformation->activateWindow();
  ui->dockWidgetInformation->raise();
  ui->tabWidgetInformation->setCurrentIndex(ic::INFO_WEATHER);
  ui->textBrowserWeatherInfo->setFocus();
}

void MainWindow::actionShortcutNavaidInformationTriggered()
{
  qDebug() << Q_FUNC_INFO;
  ui->dockWidgetInformation->show();
  ui->dockWidgetInformation->activateWindow();
  ui->dockWidgetInformation->raise();
  ui->tabWidgetInformation->setCurrentIndex(ic::INFO_NAVAID);
  ui->textBrowserNavaidInfo->setFocus();
}

void MainWindow::actionShortcutAircraftProgressTriggered()
{
  qDebug() << Q_FUNC_INFO;
  ui->dockWidgetAircraft->show();
  ui->dockWidgetAircraft->activateWindow();
  ui->dockWidgetAircraft->raise();
  ui->tabWidgetAircraft->setCurrentIndex(ic::AIRCRAFT_USER_PROGRESS);
  ui->textBrowserAircraftProgressInfo->setFocus();
}

/* Update the info weather */
void MainWindow::weatherUpdateTimeout()
{
  // if(connectClient != nullptr && connectClient->isConnected() && infoController != nullptr)
  infoController->updateAirportWeather();
}

void MainWindow::themeMenuTriggered(bool checked)
{
  QAction *action = dynamic_cast<QAction *>(sender());
  if(action != nullptr && checked)
    mapThemeComboBox->setCurrentIndex(action->data().toInt());
}

/* Look for new directories with a valid DGML file in the earth dir */
void MainWindow::findCustomMaps(QFileInfoList& customDgmlFiles)
{
  QDir dir(atools::buildPath({QCoreApplication::applicationDirPath(), "data", "maps", "earth"}));

  qDebug() << Q_FUNC_INFO << "Looking for custom maps in" << dir;

  QFileInfoList entries = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
  // Look for new map themes
  for(const QFileInfo& info : entries)
  {
    if(!STOCK_MAP_THEMES.contains(info.baseName(), Qt::CaseInsensitive))
    {
      // Found a new directory
      qDebug() << "Custom theme" << info.absoluteFilePath();
      QFileInfo dgml(info.filePath() + QDir::separator() + info.baseName() + ".dgml");
      // Check if DGML exists
      if(dgml.exists() && dgml.isFile() && dgml.isReadable())
      {
        // Create a new entry with path relative to "earth"
        QString dgmlFileRelative(QString("earth") +
                                 QDir::separator() + dir.relativeFilePath(info.absoluteFilePath()) +
                                 QDir::separator() + info.baseName() + ".dgml");
        qInfo() << "Custom theme DGML" << dgml.absoluteFilePath() << "relative path" << dgmlFileRelative;
        customDgmlFiles.append(dgmlFileRelative);
      }
      else
        qWarning() << "No DGML found for custom theme" << info.absoluteFilePath();
    }
    else
      qDebug() << "Found stock theme" << info.baseName();
  }
}

/* Called by the toolbar combo box */
void MainWindow::changeMapProjection(int index)
{
  Q_UNUSED(index);

  mapWidget->cancelDragAll();

  Marble::Projection proj = static_cast<Marble::Projection>(mapProjectionComboBox->currentData().toInt());
  qDebug() << "Changing projection to" << proj;
  mapWidget->setProjection(proj);

  // Update menu items
  ui->actionMapProjectionMercator->setChecked(proj == Marble::Mercator);
  ui->actionMapProjectionSpherical->setChecked(proj == Marble::Spherical);

  setStatusMessage(tr("Map projection changed to %1").arg(mapProjectionComboBox->currentText()));
}

/* Called by the toolbar combo box */
void MainWindow::changeMapTheme()
{
  mapWidget->cancelDragAll();

  QString theme = mapThemeComboBox->currentData().toString();
  int index = mapThemeComboBox->currentIndex();
  qDebug() << "Changing theme to" << theme << "index" << index;
  mapWidget->setTheme(theme, index);

  for(QAction *action : actionGroupMapTheme->actions())
    action->setChecked(action->data() == index);

  for(int i = 0; i < customMapThemeMenuActions.size(); i++)
    customMapThemeMenuActions.at(i)->setChecked(index == map::CUSTOM + i);

  updateLegend();

  setStatusMessage(tr("Map theme changed to %1").arg(mapThemeComboBox->currentText()));
}

void MainWindow::updateLegend()
{
  Marble::LegendWidget *legendWidget = new Marble::LegendWidget(this);
  legendWidget->setMarbleModel(mapWidget->model());
  QString basePath;
  QString html = legendWidget->getHtml(basePath);
  ui->textBrowserLegendInfo->setSearchPaths({basePath});
  ui->textBrowserLegendInfo->setText(html);
  delete legendWidget;
}

/* Menu item */
void MainWindow::showDatabaseFiles()
{
  QUrl url = QUrl::fromLocalFile(NavApp::getDatabaseManager()->getDatabaseDirectory());

  if(!QDesktopServices::openUrl(url))
    atools::gui::Dialog::warning(this, tr("Error opening help URL \"%1\"").arg(url.toDisplayString()));
}

/* Updates label and tooltip for connection status */
void MainWindow::setConnectionStatusMessageText(const QString& text, const QString& tooltipText)
{
  connectionStatus = text;
  connectionStatusTooltip = tooltipText;
  updateConnectionStatusMessageText();
}

void MainWindow::setOnlineConnectionStatusMessageText(const QString& text, const QString& tooltipText)
{
  onlineConnectionStatus = text;
  onlineConnectionStatusTooltip = tooltipText;
  updateConnectionStatusMessageText();
}

void MainWindow::updateConnectionStatusMessageText()
{
  if(onlineConnectionStatus.isEmpty())
    connectStatusLabel->setText(connectionStatus);
  else
    connectStatusLabel->setText(tr("%1/%2").arg(connectionStatus).arg(onlineConnectionStatus));

  if(onlineConnectionStatusTooltip.isEmpty())
    connectStatusLabel->setToolTip(connectionStatusTooltip);
  else
    connectStatusLabel->setToolTip(tr("Simulator:\n%1\n\nOnline Network:\n%2").
                                   arg(connectionStatusTooltip).arg(onlineConnectionStatusTooltip));
}

/* Updates label and tooltip for objects shown on map */
void MainWindow::setMapObjectsShownMessageText(const QString& text, const QString& tooltipText)
{
  messageLabel->setText(text);
  messageLabel->setToolTip(tooltipText);
}

const ElevationModel *MainWindow::getElevationModel()
{
  return mapWidget->model()->elevationModel();
}

/* Called after each query */
void MainWindow::resultTruncated(int truncatedTo)
{
  if(truncatedTo > 0)
    messageLabel->setText(atools::util::HtmlBuilder::errorMessage(tr("Too many objects.")));
}

void MainWindow::distanceChanged()
{
  // #ifdef DEBUG_INFORMATION
  // qDebug() << Q_FUNC_INFO << "minimumZoom" << mapWidget->minimumZoom() << "maximumZoom" << mapWidget->maximumZoom()
  // << "step" << mapWidget->zoomStep() << "distance" << mapWidget->distance() << "zoom" << mapWidget->zoom();
  // #endif
  float dist = Unit::distMeterF(static_cast<float>(mapWidget->distance() * 1000.f));
  QString distStr = QLocale().toString(dist, 'f', dist < 20.f ? (dist < 0.2f ? 2 : 1) : 0);
  if(distStr.endsWith(QString(QLocale().decimalPoint()) + "0"))
    distStr.chop(2);

  QString text = distStr + " " + Unit::getUnitDistStr();

#ifdef DEBUG_INFORMATION
  text += QString(" [%1]").arg(mapWidget->zoom());
#endif

  mapDistanceLabel->setText(text);
}

void MainWindow::renderStatusChanged(RenderStatus status)
{
  QString prefix =
    mapWidget->model()->workOffline() ? atools::util::HtmlBuilder::errorMessage(tr("Offline.")) : QString();

  switch(status)
  {
    case Marble::Complete:
      renderStatusLabel->setText(prefix + tr("Done."));
      break;
    case Marble::WaitingForUpdate:
      renderStatusLabel->setText(prefix + tr("Waiting for Update ..."));
      break;
    case Marble::WaitingForData:
      renderStatusLabel->setText(prefix + tr("Waiting for Data ..."));
      break;
    case Marble::Incomplete:
      renderStatusLabel->setText(prefix + tr("Incomplete."));
      break;
  }
}

/* Route center action */
void MainWindow::routeCenter()
{
  if(!NavApp::getRouteConst().isFlightplanEmpty())
  {
    mapWidget->showRect(routeController->getBoundingRect(), false);
    setStatusMessage(tr("Flight plan shown on map."));
  }
}

bool MainWindow::routeSaveCheckFMS11Warnings()
{
  if(NavApp::getDatabaseAiracCycleNav().isEmpty() &&
     NavApp::getRouteConst().getFlightplan().getFileFormat() == atools::fs::pln::FMS11)
  {
    int result = dialog->showQuestionMsgBox(lnm::ACTIONS_SHOWROUTE_NO_CYCLE_WARNING,
                                            tr(
                                              "Database contains no AIRAC cycle information which is "
                                              "required for the X-Plane FSM 11 flight plan format.<br/><br/>"
                                              "This can happen if you save a flight plan based on FSX or Prepar3D scenery.<br/><br/>"
                                              "Really continue?"),
                                            tr("Do not &show this dialog again and save in the future."),
                                            QMessageBox::Yes | QMessageBox::No,
                                            QMessageBox::No, QMessageBox::Yes);

    if(result == QMessageBox::No)
      return false;
  }
  return true;
}

/* Display warning dialogs depending on format to save and allow to cancel out or save as */
bool MainWindow::routeSaveCheckWarnings(bool& saveAs, atools::fs::pln::FileFormat fileFormat)
{
  // Show a simple warning for the rare case that altitude is null
  if(atools::almostEqual(NavApp::getRoute().getCruisingAltitudeFeet(), 0.f, 1.f))
  {
    QString message = tr("Flight plan cruise altitude is zero.\nSimulator might not be able to load the flight plan.");

    atools::gui::Dialog(nullptr).showInfoMsgBox(lnm::ACTIONS_SHOW_CRUISE_ZERO_WARNING, message,
                                                QObject::tr("Do not &show this dialog again."));
  }

  // Use a button box including a save as button
  atools::gui::DialogButtonList buttonList =
  {
    {QString(), QMessageBox::Cancel},
    // {tr("Save &as PLN instead ..."), QMessageBox::SaveAll},
    // {QString(), QMessageBox::Save},
    {QString(), QMessageBox::Help}
  };

  bool airways = routeController->getRoute().hasAirways();
  bool userWaypoints = routeController->getRoute().hasUserWaypoints();
  bool procedures = routeController->getRoute().hasAnyProcedure();
  bool parking = routeController->getRoute().hasDepartureParking();
  QString routeFilename = routeController->getCurrentRouteFilepath();
  int result = QMessageBox::Save;

  // Build button text depending if this is a "save as" or plain save
  QString saveAsButtonText = tr("Save%1%3%2").
                             arg(saveAs ? tr(" as") : QString()).
                             arg(saveAs ? tr(" ...") : QString());

  if(QFileInfo::exists(routeFilename) &&
     (fileFormat == atools::fs::pln::PLN_FS9 || fileFormat == atools::fs::pln::PLN_FSC))
  {
    // Warn before overwriting FS9 with FSX format
    buttonList.append({saveAsButtonText.arg(tr(" &FSX/P3D PLN")), QMessageBox::Save});

    QString fmt;
    if(fileFormat == atools::fs::pln::PLN_FS9)
      fmt = tr("FS9");
    else if(fileFormat == atools::fs::pln::PLN_FSC)
      fmt = tr("FSC");

    // We can load FS9 but saving does not make sense anymore
    // Ask before overwriting file
    result = atools::gui::Dialog(this).
             showQuestionMsgBox(lnm::ACTIONS_SHOW_FS9_FSC_WARNING,
                                tr("Overwrite %1 flight plan with the FSX/P3D PLN flight plan format?\n").arg(fmt),
                                tr("Do not show this dialog again and overwrite the Flight Plan in the future."),
                                buttonList, QMessageBox::Cancel, QMessageBox::Save);
  }
  else if(fileFormat == atools::fs::pln::FMS3 && (airways || procedures || parking))
  {
    buttonList.append({saveAsButtonText.arg(tr(" &FMS 3")), QMessageBox::Save});
    buttonList.append({tr("Save &as PLN instead ..."), QMessageBox::SaveAll});

    // Ask before saving file
    result =
      dialog->showQuestionMsgBox(lnm::ACTIONS_SHOW_FMS3_WARNING,
                                 tr("The old X-Plane FMS format version 3 does not allow saving of:"
                                    "<ul>"
                                      "<li>Procedures</li>"
                                        "<li>Airways</li>"
                                          "<li>Ground Speed</li>"
                                            "<li>Departure parking position</li>"
                                              "<li>Types (IFR/VFR, Low Alt/High Alt)</li>"
                                              "</ul>"
                                              "This information will be lost when reloading the file.<br/><br/>"
                                              "Really save as FMS file?<br/>"),
                                 tr("Do not show this dialog again and save the Flight Plan in the future as FMS 3."),
                                 buttonList, QMessageBox::Cancel, QMessageBox::Save);
  }
  else if(fileFormat == atools::fs::pln::FMS11 /* && parking*/) // Always warn
  {
    buttonList.append({saveAsButtonText.arg(tr(" &FMS 11")), QMessageBox::Save});
    buttonList.append({tr("Save &as PLN instead ..."), QMessageBox::SaveAll});

    // Ask before saving file
    result =
      dialog->showQuestionMsgBox(lnm::ACTIONS_SHOW_FMS11_WARNING,
                                 tr("This format can only be loaded from "
                                    "X-Plane 11.10 and above.<br/>"
                                    "It does not allow saving of:"
                                    "<ul>"
                                      "<li>Ground Speed</li>"
                                        "<li>Departure parking position</li>"
                                          "<li>Types (IFR/VFR, Low Alt/High Alt)</li>"
                                          "</ul>"
                                          "This information will be lost when reloading the file.<br/><br/>"
                                          "Really save as FMS file?<br/>"),
                                 tr("Do not show this dialog again and save the Flight Plan in the future as FMS 11."),
                                 buttonList, QMessageBox::Cancel, QMessageBox::Save);
  }
  else if(fileFormat == atools::fs::pln::FLP && (procedures || userWaypoints || parking))
  {
    buttonList.append({saveAsButtonText.arg(tr(" &FLP")), QMessageBox::Save});
    buttonList.append({tr("Save &as PLN instead ..."), QMessageBox::SaveAll});

    // Ask before saving file
    result =
      dialog->showQuestionMsgBox(lnm::ACTIONS_SHOW_FLP_WARNING,
                                 tr("The FLP format does not allow saving of:"
                                    "<ul>"
                                      "<li>Procedures (limited, can result in mismatches)</li>"
                                        "<li>Position names</li>"
                                          "<li>Cruise Altitude</li>"
                                            "<li>Ground Speed</li>"
                                              "<li>Departure parking position</li>"
                                                "<li>Types (IFR/VFR, Low Alt/High Alt)</li>"
                                                "</ul>"
                                                "This information will be lost when reloading the file.<br/><br/>"
                                                "Really save as FLP file?<br/>"),
                                 tr("Do not show this dialog again and save the Flight Plan in the future as FLP."),
                                 buttonList, QMessageBox::Cancel, QMessageBox::Save);
  }
  else if(fileFormat == atools::fs::pln::FLIGHTGEAR && (procedures || userWaypoints || parking))
  {
    buttonList.append({saveAsButtonText.arg(tr(" &FGFP")), QMessageBox::Save});
    buttonList.append({tr("Save &as PLN instead ..."), QMessageBox::SaveAll});

    // Ask before saving file
    result =
      dialog->showQuestionMsgBox(lnm::ACTIONS_SHOW_FLIGHTGEAR_WARNING,
                                 tr("The FlightGear format does not allow saving of:"
                                    "<ul>"
                                      "<li>Procedures (only SID, STAR and the respective transitions)</li>"
                                        "<li>Position names</li>"
                                          "<li>Cruise Altitude</li>"
                                            "<li>Ground Speed</li>"
                                              "<li>Departure parking position</li>"
                                                "<li>Types (IFR/VFR, Low Alt/High Alt)</li>"
                                                "</ul>"
                                                "This information will be lost when reloading the file.<br/><br/>"
                                                "Really save as FGFP file?<br/>"),
                                 tr("Do not show this dialog again and save the Flight Plan in the future as FGFP."),
                                 buttonList, QMessageBox::Cancel, QMessageBox::Save);
  }

  if(result == QMessageBox::SaveAll)
  {
    saveAs = true;
    return true;
  }
  else if(result == QMessageBox::Save)
  {
    saveAs = false;
    return true;
  }
  else if(result == QMessageBox::Help)
    atools::gui::HelpHandler::openHelpUrlWeb(this, lnm::helpOnlineUrl + "FLIGHTPLANFMT.html",
                                             lnm::helpLanguageOnline());
  // else cancel

  saveAs = false;
  return false;
}

void MainWindow::updateMapPosLabel(const atools::geo::Pos& pos, int x, int y)
{
  if(pos.isValid())
  {
    QString text(Unit::coords(pos));

    if(NavApp::getElevationProvider()->isGlobeOfflineProvider() && pos.getAltitude() < map::INVALID_ALTITUDE_VALUE)
      text += tr(" / ") + Unit::altMeter(pos.getAltitude());

    float magVar = NavApp::getMagVar(pos);
    QString magVarText = map::magvarText(magVar, true /* short text */);
#ifdef DEBUG_INFORMATION
    magVarText = QString("%1 [%2]").arg(magVarText).arg(magVar, 0, 'f', 2);
#endif

    magvarLabel->setText(magVarText);

#ifdef DEBUG_INFORMATION
    text.append(QString(" [%1,%2]").arg(x).arg(y));
#endif

    mapPosLabel->setText(text);
  }
  else
  {
    mapPosLabel->setText(tr("No position"));
    magvarLabel->clear();
  }
}

/* Updates main window title with simulator type, flight plan name and change status */
void MainWindow::updateWindowTitle()
{
  QString newTitle = mainWindowTitle;
  dm::NavdatabaseStatus navDbStatus = NavApp::getDatabaseManager()->getNavDatabaseStatus();

  atools::util::Version version(NavApp::applicationVersion());

  // Program version and revision ==========================================
  if(version.isStable() || version.isReleaseCandidate() || version.isBeta())
    newTitle += QString(" %1").arg(version.getVersionString());
  else
    newTitle += QString(" %1 (%2)").arg(version.getVersionString()).arg(GIT_REVISION);

  // Database information  ==========================================
  if(navDbStatus == dm::NAVDATABASE_ALL)
    newTitle += " - (" + NavApp::getCurrentSimulatorShortName() + ")";
  else
    newTitle += " - " + NavApp::getCurrentSimulatorShortName();

  if(navDbStatus == dm::NAVDATABASE_ALL)
    newTitle += " / N";
  else if(navDbStatus == dm::NAVDATABASE_MIXED)
    newTitle += " / N";

  // Flight plan name  ==========================================
  if(!routeController->getCurrentRouteFilepath().isEmpty())
    newTitle += " - " + QFileInfo(routeController->getCurrentRouteFilepath()).fileName() +
                (routeController->hasChanged() ? tr(" *") : QString());
  else if(routeController->hasChanged())
    newTitle += tr(" - *");

  // Performance name  ==========================================
  if(!NavApp::getCurrentAircraftPerfFilepath().isEmpty())
    newTitle += " - " + QFileInfo(NavApp::getCurrentAircraftPerfFilepath()).fileName() +
                (NavApp::getAircraftPerfController()->hasChanged() ? tr(" *") : QString());
  else if(NavApp::getAircraftPerfController()->hasChanged())
    newTitle += tr(" - *");

  // Add a star to the flight plan tab if changed
  if(routeController->hasChanged())
  {
    if(!ui->tabWidgetRoute->tabText(rc::ROUTE).endsWith(tr(" *")))
      ui->tabWidgetRoute->setTabText(rc::ROUTE, ui->tabWidgetRoute->tabText(rc::ROUTE) + tr(" *"));
  }
  else
    ui->tabWidgetRoute->setTabText(rc::ROUTE, ui->tabWidgetRoute->tabText(rc::ROUTE).replace(tr(" *"), QString()));

  setWindowTitle(newTitle);
}

void MainWindow::deleteAircraftTrack()
{
  int result = atools::gui::Dialog(this).
               showQuestionMsgBox(lnm::ACTIONS_SHOW_DELETE_TRAIL,
                                  tr("Delete aircraft trail?"),
                                  tr("Do not &show this dialog again."),
                                  QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No, QMessageBox::Yes);

  if(result == QMessageBox::Yes)
  {
    mapWidget->deleteAircraftTrack();
    profileWidget->deleteAircraftTrack();
    updateActionStates();
    setStatusMessage(QString(tr("Aircraft track removed from map.")));
  }
}

/* Ask user if flight plan can be deleted when quitting.
 * @return true continue with new flight plan, exit, etc. */
bool MainWindow::routeCheckForChanges()
{
  if(!routeController->hasChanged())
    return true;

  QMessageBox msgBox;
  msgBox.setWindowTitle(QApplication::applicationName());
  msgBox.setText(tr("Flight Plan has been changed."));
  msgBox.setInformativeText(tr("Save changes?"));
  msgBox.setStandardButtons(QMessageBox::Save | QMessageBox::No | QMessageBox::Cancel);

  int retval = msgBox.exec();

  switch(retval)
  {
    case QMessageBox::Save:
      if(routeController->getCurrentRouteFilepath().isEmpty())
        return routeSaveAsPln();
      else
        return routeSave();

    case QMessageBox::No:
      // ok to erase flight plan
      return true;

    case QMessageBox::Cancel:
      // stop any flight plan erasing actions
      return false;
  }
  return false;
}

/* Open a dialog that allows to create a new route from a string */
void MainWindow::routeNewFromString()
{
  RouteStringDialog routeStringDialog(this, routeController);
  routeStringDialog.restoreState();

  if(routeStringDialog.exec() == QDialog::Accepted)
  {
    if(!routeStringDialog.getFlightplan().isEmpty())
    {
      if(routeCheckForChanges())
      {
        routeController->loadFlightplan(routeStringDialog.getFlightplan(), QString(),
                                        true /*quiet*/, true /*changed*/,
                                        !routeStringDialog.isAltitudeIncluded() /*adjust alt*/);
        if(OptionData::instance().getFlags() & opts::GUI_CENTER_ROUTE)
          routeCenter();
        setStatusMessage(tr("Created new flight plan."));
      }
    }
    else
      qWarning() << "Flight plan is null";
  }
  routeStringDialog.saveState();
}

/* Called from menu or toolbar by action */
void MainWindow::routeNew()
{
  if(routeCheckForChanges())
  {
    routeController->newFlightplan();
    mapWidget->update();
    setStatusMessage(tr("Created new empty flight plan."));
  }
}

/* Called from menu or toolbar by action */
void MainWindow::routeOpen()
{
  routeOpenFile(QString());
}

void MainWindow::routeOpenFile(QString filepath)
{
  if(routeCheckForChanges())
  {
    if(filepath.isEmpty())
      filepath = dialog->openFileDialog(
        tr("Open Flight Plan"),
        tr("Flight Plan Files %1;;All Files (*)").arg(lnm::FILE_PATTERN_FLIGHTPLAN_LOAD),
        "Route/" + NavApp::getCurrentSimulatorShortName(),
        NavApp::getCurrentSimulatorFilesPath());

    if(!filepath.isEmpty())
    {
      if(routeController->loadFlightplan(filepath))
      {
        routeFileHistory->addFile(filepath);
        if(OptionData::instance().getFlags() & opts::GUI_CENTER_ROUTE)
          routeCenter();
        setStatusMessage(tr("Flight plan opened."));
      }
    }
  }
  saveFileHistoryStates();
}

/* Called from menu or toolbar by action - append flight plan to current one */
void MainWindow::routeAppend()
{
  QString routeFile = dialog->openFileDialog(
    tr("Append Flight Plan"),
    tr("Flight Plan Files %1;;All Files (*)").arg(lnm::FILE_PATTERN_FLIGHTPLAN_LOAD),
    "Route/" + NavApp::getCurrentSimulatorShortName(),
    NavApp::getCurrentSimulatorFilesPath());

  if(!routeFile.isEmpty())
  {
    if(routeController->insertFlightplan(routeFile, routeController->getRoute().size() /* append */))
    {
      routeFileHistory->addFile(routeFile);
      if(OptionData::instance().getFlags() & opts::GUI_CENTER_ROUTE)
        routeCenter();
      setStatusMessage(tr("Flight plan appended."));
    }
  }
  saveFileHistoryStates();
}

/* Called by route controller - insert flight plan into current one */
void MainWindow::routeInsert(int insertBefore)
{
  QString routeFile = dialog->openFileDialog(
    tr("Insert info Flight Plan"),
    tr("Flight Plan Files %1;;All Files (*)").arg(lnm::FILE_PATTERN_FLIGHTPLAN_LOAD),
    "Route/" + NavApp::getCurrentSimulatorShortName(),
    NavApp::getCurrentSimulatorFilesPath());

  if(!routeFile.isEmpty())
  {
    if(routeController->insertFlightplan(routeFile, insertBefore))
    {
      routeFileHistory->addFile(routeFile);
      if(OptionData::instance().getFlags() & opts::GUI_CENTER_ROUTE)
        routeCenter();
      setStatusMessage(tr("Flight plan inserted."));
    }
  }
  saveFileHistoryStates();
}

/* Called from menu or toolbar by action */
void MainWindow::routeOpenRecent(const QString& routeFile)
{
  if(routeCheckForChanges())
  {
    if(QFile::exists(routeFile))
    {
      if(routeController->loadFlightplan(routeFile))
      {
        if(OptionData::instance().getFlags() & opts::GUI_CENTER_ROUTE)
          routeCenter();
        setStatusMessage(tr("Flight plan opened."));
      }
    }
    else
    {
      NavApp::deleteSplashScreen();

      // File not valid remove from history
      atools::gui::Dialog::warning(this, tr("File \"%1\" does not exist").arg(routeFile));
      routeFileHistory->removeFile(routeFile);
    }
  }
  saveFileHistoryStates();
}

/* Called from menu or toolbar by action */
bool MainWindow::routeSave()
{
  atools::fs::pln::FileFormat format = NavApp::getRouteConst().getFlightplan().getFileFormat();

  if(!routeSaveCheckFMS11Warnings())
    return false;

  if(routeController->getCurrentRouteFilepath().isEmpty() || !routeController->doesFilenameMatchRoute(format))
  {
    // No filename or plan has changed - save as
    if(format == atools::fs::pln::FMS3 || format == atools::fs::pln::FMS11)
      return routeSaveAsFms(format);
    else if(format == atools::fs::pln::FLP)
      return routeSaveAsFlp();
    else if(format == atools::fs::pln::FLIGHTGEAR)
      return routeSaveAsFlightGear();
    else
      return routeSaveAsPln();
  }
  else
  {
    bool saveAs = false;
    bool save = routeSaveCheckWarnings(saveAs, routeController->getRoute().getFlightplan().getFileFormat());

    if(saveAs)
      return routeSaveAsPln();
    else if(save)
    {

      bool validate = format == atools::fs::pln::PLN_FSX || format == atools::fs::pln::PLN_FS9 ||
                      format == atools::fs::pln::PLN_FSC;
      if(routeExport->routeValidate(validate /* validate parking */, validate /* validate departure and destination */))
      {
        // Save in loaded format PLN, FLP or FMS
        if(routeController->saveFlightplan(false /* clean */))
        {
          routeFileHistory->addFile(routeController->getCurrentRouteFilepath());
          updateActionStates();
          setStatusMessage(tr("Flight plan saved."));
          saveFileHistoryStates();
          return true;
        }
      }
    }
  }
  return false;
}

/* Called from menu or toolbar by action */
bool MainWindow::routeSaveAsPln()
{
  if(routeExport->routeValidate(true /* validate parking */, true /* validate departure and destination */))
  {
    QString routeFile = dialog->saveFileDialog(
      tr("Save Flight Plan as PLN Format"),
      tr("Flight Plan Files %1;;All Files (*)").arg(lnm::FILE_PATTERN_FLIGHTPLAN_SAVE),
      "pln", "Route/" + NavApp::getCurrentSimulatorShortName(),
      NavApp::getCurrentSimulatorFilesPath(),
      (OptionData::instance().getFlags2() & opts::ROUTE_SAVE_SHORT_NAME) ?
      routeExport->buildDefaultFilenameShort("_", ".pln") : routeExport->buildDefaultFilename(),
      false /* confirm overwrite */, true /* autonumber */);

    if(!routeFile.isEmpty())
    {
      if(routeController->saveFlighplanAs(routeFile, atools::fs::pln::PLN_FSX))
      {
        routeFileHistory->addFile(routeFile);
        updateActionStates();
        setStatusMessage(tr("Flight plan saved."));
        saveFileHistoryStates();
        return true;
      }
    }
  }
  return false;
}

bool MainWindow::routeSaveAsFlp()
{
  // <Documents>/Aerosoft/Airbus/Flightplans.
  bool saveAs = true;
  bool save = routeSaveCheckWarnings(saveAs, atools::fs::pln::FLP);

  if(saveAs)
    return routeSaveAsPln();
  else if(save)
  {
    QString routeFile = dialog->saveFileDialog(
      tr("Save Flight Plan as FLP Format"),
      tr("FLP Files %1;;All Files (*)").arg(lnm::FILE_PATTERN_FLP),
      "flp", "Route/Flp", QStandardPaths::standardLocations(QStandardPaths::DocumentsLocation).first(),
      routeExport->buildDefaultFilenameShort(QString(), ".flp"),
      false /* confirm overwrite */, true /* autonumber */);

    if(!routeFile.isEmpty())
    {
      if(routeController->saveFlighplanAs(routeFile, atools::fs::pln::FLP))
      {
        routeFileHistory->addFile(routeFile);
        updateActionStates();
        setStatusMessage(tr("Flight plan saved as FLP."));
        return true;
      }
    }
  }
  return false;
}

bool MainWindow::routeSaveAsFlightGear()
{
  bool saveAs = true;
  bool save = routeSaveCheckWarnings(saveAs, atools::fs::pln::FLIGHTGEAR);

  if(saveAs)
    return routeSaveAsPln();
  else if(save)
  {
    QString routeFile = dialog->saveFileDialog(
      tr("Save Flight Plan as FlightGear Format"),
      tr("FlightGear Files %1;;All Files (*)").arg(lnm::FILE_PATTERN_FLIGHTGEAR),
      "fgfp", "Route/FlightGear", QStandardPaths::standardLocations(QStandardPaths::DocumentsLocation).first(),
      (OptionData::instance().getFlags2() & opts::ROUTE_SAVE_SHORT_NAME) ?
      routeExport->buildDefaultFilenameShort("_", ".fgfp") : routeExport->buildDefaultFilename(QString(), ".fgfp"),
      false /* confirm overwrite */, true /* autonumber */);

    if(!routeFile.isEmpty())
    {
      if(routeController->saveFlighplanAs(routeFile, atools::fs::pln::FLIGHTGEAR))
      {
        routeFileHistory->addFile(routeFile);
        updateActionStates();
        setStatusMessage(tr("Flight plan saved for FlightGear."));
        return true;
      }
    }
  }
  return false;
}

bool MainWindow::routeSaveAsFms3()
{
  return routeSaveAsFms(atools::fs::pln::FMS3);
}

bool MainWindow::routeSaveAsFms11()
{
  return routeSaveAsFms(atools::fs::pln::FMS11);
}

bool MainWindow::routeSaveAsFms(atools::fs::pln::FileFormat format)
{
  bool saveAs = true;

  if(!routeSaveCheckFMS11Warnings())
    return false;

  bool save = routeSaveCheckWarnings(saveAs, format);

  if(saveAs)
    return routeSaveAsPln();
  else if(save)
  {
    // Try to get X-Plane default output directory for flight plans
    QString xpBasePath = NavApp::getSimulatorBasePath(atools::fs::FsPaths::XPLANE11);
    if(xpBasePath.isEmpty())
      xpBasePath = QStandardPaths::standardLocations(QStandardPaths::DocumentsLocation).first();
    else
      xpBasePath = atools::buildPathNoCase({xpBasePath, "Output", "FMS plans"});

    QString routeFile = dialog->saveFileDialog(
      tr("Save Flight Plan as X-Plane FMS Format"),
      tr("FMS Files %1;;All Files (*)").arg(lnm::FILE_PATTERN_FMS),
      "fms", "Route/Fms", xpBasePath,
      routeExport->buildDefaultFilenameShort(QString(), ".fms"),
      false /* confirm overwrite */, true /* autonumber */);

    if(!routeFile.isEmpty())
    {
      if(routeController->saveFlighplanAs(routeFile, format))
      {
        routeFileHistory->addFile(routeFile);
        updateActionStates();
        setStatusMessage(tr("Flight plan saved as FMS."));
        saveFileHistoryStates();
        return true;
      }
    }
  }
  return false;
}

bool MainWindow::routeExportClean()
{
  if(routeExport->routeValidate(true /* validate parking */, true /* validate departure and destination */))
  {
    QString routeFile = dialog->saveFileDialog(
      tr("Save Clean Flight Plan without Annotations"),
      tr("Flight Plan Files %1;;All Files (*)").arg(lnm::FILE_PATTERN_FLIGHTPLAN_SAVE),
      "pln", "Route/Clean" + NavApp::getCurrentSimulatorShortName(), NavApp::getCurrentSimulatorFilesPath(),
      (OptionData::instance().getFlags2() & opts::ROUTE_SAVE_SHORT_NAME) ?
      routeExport->buildDefaultFilenameShort("_", ".pln") : routeExport->buildDefaultFilename(tr(" Clean")),
      false /* confirm overwrite */, true /* autonumber */);

    if(!routeFile.isEmpty())
    {
      if(routeController->exportFlighplanAsClean(routeFile))
      {
        setStatusMessage(tr("Flight plan exported."));
        return true;
      }
    }
  }
  return false;
}

bool MainWindow::openInSkyVector()
{
  // https://skyvector.com/?fpl=%20EDDH%20AMLU7C%20AMLUH%20M852%20POVEL%20GALMA%20UM736%20DOSEL%20DETSA%20NIKMA%20T369%20RITEB%20RITE4B%20LIRF

  QString route =
    RouteString::createStringForRoute(NavApp::getRoute(),
                                      NavApp::getRouteCruiseSpeedKts(),
                                      rs::START_AND_DEST | rs::SKYVECTOR_COORDS);

  HelpHandler::openUrlWeb(this, "https://skyvector.com/?fpl=" + route);
  return true;
}

/* Called from menu or toolbar by action. Remove all KML from map */
void MainWindow::kmlClear()
{
  mapWidget->clearKmlFiles();
  updateActionStates();
  setStatusMessage(tr("Google Earth KML files removed from map."));
}

/* Called from menu or toolbar by action */
void MainWindow::kmlOpen()
{
  QString kmlFile = dialog->openFileDialog(
    tr("Google Earth KML"),
    tr("Google Earth KML %1;;All Files (*)").arg(lnm::FILE_PATTERN_KML),
    "Kml/", QString());

  if(!kmlFile.isEmpty())
  {
    if(mapWidget->addKmlFile(kmlFile))
    {
      kmlFileHistory->addFile(kmlFile);
      updateActionStates();
      setStatusMessage(tr("Google Earth KML file opened."));
    }
    else
      setStatusMessage(tr("Opening Google Earth KML file failed."));

  }
}

/* Called from menu or toolbar by action */
void MainWindow::kmlOpenRecent(const QString& kmlFile)
{
  if(mapWidget->addKmlFile(kmlFile))
  {
    updateActionStates();
    setStatusMessage(tr("Google Earth KML file opened."));
  }
  else
  {
    kmlFileHistory->removeFile(kmlFile);
    setStatusMessage(tr("Opening Google Earth KML file failed."));
  }
}

void MainWindow::mapSaveImage()
{
  QString imageFile = dialog->saveFileDialog(
    tr("Save Map as Image"), tr("Image Files %1;;All Files (*)").arg(lnm::FILE_PATTERN_IMAGE),
    "jpg", "MainWindow/",
    atools::fs::FsPaths::getFilesPath(NavApp::getCurrentSimulatorDb()), tr("Little Navmap Map %1.jpg").
    arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss")));

  if(!imageFile.isEmpty())
  {
    QPixmap pixmap;
    mapWidget->showOverlays(false, false /* show scale */);
    pixmap = mapWidget->mapScreenShot();
    mapWidget->showOverlays(true, false /* show scale */);

    PrintSupport::drawWatermark(QPoint(0, pixmap.height()), &pixmap);

    if(!pixmap.save(imageFile))
      atools::gui::Dialog::warning(this, tr("Error saving image.\n" "Only JPG, PNG and BMP are allowed."));
    else
      setStatusMessage(tr("Map image saved."));
  }
}

void MainWindow::mapSaveImageAviTab()
{
  if(mapWidget->projection() == Marble::Mercator)
  {
    QString json = mapWidget->createAvitabJson();

    if(!json.isEmpty())
    {
      QString defaultFileName;

      if(routeController->getRoute().isEmpty())
        defaultFileName = tr("LittleNavmap_%1.png").arg(QDateTime::currentDateTime().toString("yyyyMMddHHmm"));
      else
        defaultFileName = routeExport->buildDefaultFilenameShort("_", ".png");

      QString imageFile = dialog->saveFileDialog(
        tr("Save Map as Image for AviTab"),
        tr("AviTab Image Files %1;;All Files (*)").arg(lnm::FILE_PATTERN_IMAGE_AVITAB),
        "png", "MainWindow/AviTab",
        atools::fs::FsPaths::getBasePath(NavApp::getCurrentSimulatorDb()) +
        QDir::separator() + "Resources" + QDir::separator() + "plugins" + QDir::separator() + "AviTab" +
        QDir::separator() + "MapTiles" + QDir::separator() + "Mercator", defaultFileName,
        false /* confirm overwrite */, true /* autonumber file */);

      if(!imageFile.isEmpty())
      {
        QPixmap pixmap;
        mapWidget->showOverlays(false, false /* show scale */);
        pixmap = mapWidget->mapScreenShot();
        mapWidget->showOverlays(true, false /* show scale */);

        if(!pixmap.save(imageFile))
          atools::gui::Dialog::warning(this, tr("Error saving image.\n" "Only JPG and PNG are allowed."));
        else
        {
          QFile jsonFile(imageFile + ".json");
          if(jsonFile.open(QIODevice::WriteOnly | QIODevice::Text))
          {
            QTextStream stream(&jsonFile);
            stream << json.toUtf8();

            setStatusMessage(tr("Map image saved."));
          }
          else
            atools::gui::ErrorHandler(this).handleIOError(jsonFile, tr("Error saving JSON."));
        }
      }
    }
  }
  else
    QMessageBox::warning(this, QApplication::applicationName(),
                         tr("You have to switch to the Mercator map projection before saving the image."));
}

void MainWindow::mapCopyToClipboard()
{
  QPixmap pixmap;
  mapWidget->showOverlays(false, false /* show scale */);
  pixmap = mapWidget->mapScreenShot();
  mapWidget->showOverlays(true, false /* show scale */);

  PrintSupport::drawWatermark(QPoint(0, pixmap.height()), &pixmap);

  // Copy formatted and plain text to clipboard
  QMimeData *data = new QMimeData;
  data->setImageData(pixmap);
  QGuiApplication::clipboard()->setMimeData(data);
  setStatusMessage(tr("Map image copied to clipboard."));
}

void MainWindow::sunShadingTimeChanged()
{
  qDebug() << Q_FUNC_INFO;

  if(ui->actionMapShowSunShadingRealTime->isChecked() || ui->actionMapShowSunShadingUserTime->isChecked())
    // User time defaults to current time until set
    mapWidget->setSunShadingDateTime(QDateTime::currentDateTimeUtc());
  else if(ui->actionMapShowSunShadingSimulatorTime->isChecked())
  {
    if(!NavApp::isConnectedAndAircraft())
      // Use current time if not connected
      mapWidget->setSunShadingDateTime(QDateTime::currentDateTimeUtc());
    // else  Updated by simDataChanged
  }

  mapWidget->updateSunShadingOption();
}

void MainWindow::sunShadingTimeSet()
{
  qDebug() << Q_FUNC_INFO;

  // Initalize dialog with current time
  TimeDialog timeDialog(this, mapWidget->getSunShadingDateTime());
  timeDialog.exec();
}

/* Selection in flight plan table has changed */
void MainWindow::routeSelectionChanged(int selected, int total)
{
  Q_UNUSED(selected);
  Q_UNUSED(total);
  QList<int> result;
  routeController->getSelectedRouteLegs(result);
  mapWidget->changeRouteHighlights(result);
  updateHighlightActionStates();
}

/* Selection in one of the search result tables has changed */
void MainWindow::searchSelectionChanged(const SearchBaseTable *source, int selected, int visible, int total)
{
  QString selectionLabelText = tr("%1 of %2 %3 selected, %4 visible.%5");
  QString type;
  if(source->getTabIndex() == si::SEARCH_AIRPORT)
  {
    type = tr("Airports");
    ui->labelAirportSearchStatus->setText(selectionLabelText.
                                          arg(selected).arg(total).arg(type).arg(visible).arg(QString()));
  }
  else if(source->getTabIndex() == si::SEARCH_NAV)
  {
    type = tr("Navaids");
    ui->labelNavSearchStatus->setText(selectionLabelText.
                                      arg(selected).arg(total).arg(type).arg(visible).arg(QString()));
  }
  else if(source->getTabIndex() == si::SEARCH_USER)
  {
    type = tr("Userpoints");
    ui->labelUserdata->setText(selectionLabelText.
                               arg(selected).arg(total).arg(type).arg(visible).arg(QString()));
  }
  else if(source->getTabIndex() == si::SEARCH_ONLINE_CLIENT)
  {
    type = tr("Clients");
    QString lastUpdate = tr(" Last Update: %1").
                         arg(NavApp::getOnlinedataController()->getLastUpdateTime().toString(Qt::SystemLocaleShortDate));
    ui->labelOnlineClientSearchStatus->setText(selectionLabelText.
                                               arg(selected).arg(total).arg(type).arg(visible).
                                               arg(lastUpdate));
  }
  else if(source->getTabIndex() == si::SEARCH_ONLINE_CENTER)
  {
    type = tr("Centers");
    QString lastUpdate = tr(" Last Update: %1").
                         arg(NavApp::getOnlinedataController()->getLastUpdateTime().toString(Qt::SystemLocaleShortDate));
    ui->labelOnlineCenterSearchStatus->setText(selectionLabelText.
                                               arg(selected).arg(total).arg(type).arg(visible).
                                               arg(lastUpdate));
  }

  map::MapSearchResult result;
  searchController->getSelectedMapObjects(result);
  mapWidget->changeSearchHighlights(result);
  updateHighlightActionStates();
}

/* Selection in approach view has changed */
void MainWindow::procedureSelected(const proc::MapProcedureRef& ref)
{
  // qDebug() << Q_FUNC_INFO << "approachId" << ref.approachId
  // << "transitionId" << ref.transitionId
  // << "legId" << ref.legId;

  map::MapAirport airport = NavApp::getAirportQueryNav()->getAirportById(ref.airportId);

  if(ref.isEmpty())
    mapWidget->changeApproachHighlight(proc::MapProcedureLegs());
  else
  {
    if(ref.hasApproachAndTransitionIds())
    {
      // Load transition including approach
      const proc::MapProcedureLegs *legs = NavApp::getProcedureQuery()->getTransitionLegs(airport, ref.transitionId);
      if(legs != nullptr)
        mapWidget->changeApproachHighlight(*legs);
      else
        qWarning() << "Transition not found" << ref.transitionId;
    }
    else if(ref.hasApproachOnlyIds())
    {
      proc::MapProcedureRef curRef = mapWidget->getProcedureHighlight().ref;
      if(ref.airportId == curRef.airportId && ref.approachId == curRef.approachId &&
         !ref.hasTransitionId() && curRef.hasTransitionId() && ref.isLeg())
      {
        // Approach leg selected - keep preview of current transition
        proc::MapProcedureRef r = ref;
        r.transitionId = curRef.transitionId;
        const proc::MapProcedureLegs *legs = NavApp::getProcedureQuery()->getTransitionLegs(airport, r.transitionId);
        if(legs != nullptr)
          mapWidget->changeApproachHighlight(*legs);
        else
          qWarning() << "Transition not found" << r.transitionId;
      }
      else
      {
        const proc::MapProcedureLegs *legs = NavApp::getProcedureQuery()->getApproachLegs(airport, ref.approachId);
        if(legs != nullptr)
          mapWidget->changeApproachHighlight(*legs);
        else
          qWarning() << "Approach not found" << ref.transitionId;
      }
    }
  }
  updateHighlightActionStates();
}

/* Selection in approach view has changed */
void MainWindow::procedureLegSelected(const proc::MapProcedureRef& ref)
{
  // qDebug() << Q_FUNC_INFO << "approachId" << ref.approachId
  // << "transitionId" << ref.transitionId
  // << "legId" << ref.legId;

  if(ref.legId != -1)
  {
    const proc::MapProcedureLeg *leg;

    map::MapAirport airport = NavApp::getAirportQueryNav()->getAirportById(ref.airportId);
    if(ref.transitionId != -1)
      leg = NavApp::getProcedureQuery()->getTransitionLeg(airport, ref.legId);
    else
      leg = NavApp::getProcedureQuery()->getApproachLeg(airport, ref.approachId, ref.legId);

    if(leg != nullptr)
    {
      // qDebug() << *leg;

      mapWidget->changeProcedureLegHighlights(leg);
    }
  }
  else
    mapWidget->changeProcedureLegHighlights(nullptr);
  updateHighlightActionStates();
}

void MainWindow::updateAirspaceTypes(map::MapAirspaceFilter types)
{
  mapWidget->setShowMapAirspaces(types);
  mapWidget->updateMapObjectsShown();
  setStatusMessage(tr("Map settings changed."));
}

void MainWindow::resetMapObjectsShown()
{
  qDebug() << Q_FUNC_INFO;

  mapWidget->resetSettingActionsToDefault();
  mapWidget->resetSettingsToDefault();
  NavApp::getUserdataController()->resetSettingsToDefault();

  mapWidget->updateMapObjectsShown();
  airspaceHandler->updateButtonsAndActions();
  profileWidget->update();
  setStatusMessage(tr("Map settings changed."));
}

/* A button like airport, vor, ndb, etc. was pressed - update the map */
void MainWindow::updateMapObjectsShown()
{
  // Save to configuration
  // saveActionStates();

  mapWidget->updateMapObjectsShown();
  profileWidget->update();
  setStatusMessage(tr("Map settings changed."));
}

/* Map history has changed */
void MainWindow::updateMapHistoryActions(int minIndex, int curIndex, int maxIndex)
{
  ui->actionMapBack->setEnabled(curIndex > minIndex);
  ui->actionMapNext->setEnabled(curIndex < maxIndex);
}

/* Reset all "do not show this again" message box status values */
void MainWindow::resetMessages()
{
  qDebug() << "resetMessages";
  Settings& s = Settings::instance();

  // Show all message dialogs again
  s.setValue(lnm::ACTIONS_SHOW_DISCONNECT_INFO, true);
  s.setValue(lnm::ACTIONS_SHOW_LOAD_FLP_WARN, true);
  s.setValue(lnm::ACTIONS_SHOW_QUIT, true);
  s.setValue(lnm::ACTIONS_SHOW_INVALID_PROC_WARNING, true);
  s.setValue(lnm::ACTIONS_SHOW_RESET_VIEW, true);
  s.setValue(lnm::ACTIONS_SHOWROUTE_PARKING_WARNING, true);
  s.setValue(lnm::ACTIONS_SHOWROUTE_WARNING, true);
  s.setValue(lnm::ACTIONS_SHOWROUTE_ERROR, true);
  s.setValue(lnm::ACTIONS_SHOWROUTE_PROC_ERROR, true);
  s.setValue(lnm::ACTIONS_SHOWROUTE_START_CHANGED, true);
  s.setValue(lnm::OPTIONS_DIALOG_WARN_STYLE, true);

  s.setValue(lnm::ACTIONS_SHOW_FS9_FSC_WARNING, true);
  s.setValue(lnm::ACTIONS_SHOW_FLP_WARNING, true);
  s.setValue(lnm::ACTIONS_SHOW_FMS3_WARNING, true);
  s.setValue(lnm::ACTIONS_SHOW_FMS11_WARNING, true);
  s.setValue(lnm::ACTIONS_SHOW_FLIGHTGEAR_WARNING, true);
  s.setValue(lnm::ACTIONS_SHOW_UPDATE_FAILED, true);
  s.setValue(lnm::ACTIONS_SHOW_SSL_FAILED, true);
  s.setValue(lnm::ACTIONS_SHOW_OVERWRITE_DATABASE, true);
  s.setValue(lnm::ACTIONS_SHOW_NAVDATA_WARNING, true);
  s.setValue(lnm::ACTIONS_SHOW_CRUISE_ZERO_WARNING, true);
  s.setValue(lnm::ACTIONS_SHOW_INSTALL_GLOBE, true);
  s.setValue(lnm::ACTIONS_SHOW_START_PERF_COLLECTION, true);
  s.setValue(lnm::ACTIONS_SHOW_DELETE_TRAIL, true);

  setStatusMessage(tr("All message dialogs reset."));
}

/* Set a general status message */
void MainWindow::setStatusMessage(const QString& message)
{
  if(statusMessages.isEmpty() || statusMessages.last() != message)
    statusMessages.append(message);

  if(statusMessages.size() > 1)
    statusMessages.removeAt(0);

  ui->statusBar->showMessage(statusMessages.join(" "));
}

void MainWindow::setDetailLabelText(const QString& text)
{
  detailLabel->setText(text);
}

/* From menu: Show options dialog */
void MainWindow::options()
{
  int retval = optionsDialog->exec();
  optionsDialog->hide();

  // Dialog saves its own states
  if(retval == QDialog::Accepted)
    setStatusMessage(tr("Options changed."));
}

/* Called by window shown event when the main window is visible the first time */
void MainWindow::mainWindowShown()
{
  qDebug() << Q_FUNC_INFO << "enter";

  // Postpone loading of KML etc. until now when everything is set up
  mapWidget->mainWindowShown();
  profileWidget->mainWindowShown();

  mapWidget->showSavedPosOnStartup();

  // Show a warning if SSL was not intiaized properly. Can happen if the redist packages are not installed.
  if(!QSslSocket::supportsSsl())
  {
    NavApp::deleteSplashScreen();

    QUrl url = atools::gui::HelpHandler::getHelpUrlWeb(lnm::helpOnlineInstallRedistUrl, lnm::helpLanguageOnline());
    QString message = QObject::tr("<p>Error initializing SSL subsystem.</p>"
                                    "<p>The program will not be able to use encrypted network connections<br/>"
                                    "(i.e. HTTPS) that are needed to check for updates or<br/>"
                                    "to load online maps.</p>");

#if defined(Q_OS_WIN32)
    QString message2 = QObject::tr("<p><b>Click the link below for more information:<br/><br/>"
                                   "<a href=\"%1\">Online Manual - Installation</a></b><br/></p>").
                       arg(url.toString());
    message += message2;
#endif

    atools::gui::Dialog(nullptr).showWarnMsgBox(lnm::ACTIONS_SHOW_SSL_FAILED, message,
                                                QObject::tr("Do not &show this dialog again."));
  }

  if(!NavApp::getElevationProvider()->isGlobeOfflineProvider())
  {
    NavApp::deleteSplashScreen();

    QUrl url = atools::gui::HelpHandler::getHelpUrlWeb(lnm::helpOnlineInstallGlobeUrl, lnm::helpLanguageOnline());
    QString message = QObject::tr("<p>The online elevation data which is used by default for the elevation profile "
                                    "is limited and has a lot of errors.<br/>"
                                    "Therefore, it is recommended to download and use the offline GLOBE elevation data "
                                    "which provides world wide coverage.</p>"
                                    "<p><b>Click the link below for more information:<br/><br/>"
                                    "<a href=\"%1\">Online Manual - Options Dialog / Flight Plan Elevation Profile</a></b><br/></p>")
                      .arg(url.toString());

    atools::gui::Dialog(nullptr).showInfoMsgBox(lnm::ACTIONS_SHOW_INSTALL_GLOBE, message,
                                                QObject::tr("Do not &show this dialog again."));
  }

  DatabaseManager *databaseManager = NavApp::getDatabaseManager();
  if(firstApplicationStart)
  {
    firstApplicationStart = false;
    if(!databaseManager->hasSimulatorDatabases())
    {
      NavApp::deleteSplashScreen();

      // Show the scenery database dialog on first start
#ifdef Q_OS_WIN32
      if(databaseManager->hasInstalledSimulators())
        // No databases but simulators let the user create new databases
        databaseManager->run();
      else
      {
        // No databases and no simulators - show a message dialog
        QMessageBox msgBox(this);
        msgBox.setWindowTitle(QApplication::applicationName());
        msgBox.setTextFormat(Qt::RichText);
        msgBox.setText(
          tr("Could not find a<ul>"
             "<li>Microsoft Flight Simulator X,</li>"
               "<li>Flight Simulator - Steam Edition or</li>"
                 "<li>Prepar3D installation</li></ul>"
                   "on this computer. Also, no scenery library databases were found.<br/><br/>"
                   "You can copy a Little Navmap scenery library database from another computer.<br/>"
                   "Press the help button for more information on this.<br/><br/>"
                   "If you have X-Plane 11 installed you can go to the scenery library "
                   "loading dialog by clicking the X-Plane button below.<br/><br/>"
             )
          );
        msgBox.setStandardButtons(QMessageBox::Ok | QMessageBox::Open | QMessageBox::Help);
        msgBox.setButtonText(QMessageBox::Open, tr("X-Plane"));
        msgBox.setDefaultButton(QMessageBox::Ok);

        int result = msgBox.exec();
        if(result == QMessageBox::Help)
          HelpHandler::openHelpUrlWeb(this, lnm::helpOnlineUrl + "RUNNOSIM.html", lnm::helpLanguageOnline());
        else if(result == QMessageBox::Open)
          databaseManager->run();
      }
#else
      // Show always on non Windows systems
      databaseManager->run();
#endif
    }

    // else have databases do nothing
  }
  else if(databasesErased)
  {
    databasesErased = false;
    // Databases were removed - show dialog
    NavApp::deleteSplashScreen();
    databaseManager->run();
  }

  // If enabled connect to simulator without showing dialog
  NavApp::getConnectClient()->tryConnectOnStartup();

  weatherUpdateTimeout();

  // Update the weather every 15 seconds if connected
  weatherUpdateTimer.setInterval(WEATHER_UPDATE_MS);
  weatherUpdateTimer.start();

  // Check for updates once main window is visible
  NavApp::checkForUpdates(OptionData::instance().getUpdateChannels(), false /* manually triggered */);

  NavApp::getOnlinedataController()->startProcessing();

  // Raise all floating docks and focus map widget
  QTimer::singleShot(10, this, &MainWindow::raiseFloatingWindows);

  //// Workaround for profile dock widget which is not resized properly on startup
  // QTimer::singleShot(20, this, &MainWindow::adjustProfileDockHeight);

  setStatusMessage(tr("Ready."));

  qDebug() << Q_FUNC_INFO << "leave";
}

void MainWindow::raiseFloatingWindow(QDockWidget *dockWidget)
{
  if(dockWidget->isVisible() && dockWidget->isFloating())
    dockWidget->raise();
}

void MainWindow::raiseFloatingWindows()
{
  raiseFloatingWindow(ui->dockWidgetLegend);
  raiseFloatingWindow(ui->dockWidgetAircraft);
  raiseFloatingWindow(ui->dockWidgetSearch);
  raiseFloatingWindow(ui->dockWidgetProfile);
  raiseFloatingWindow(ui->dockWidgetInformation);
  raiseFloatingWindow(ui->dockWidgetRoute);

  raiseFloatingWindow(ui->dockWidgetMap);

  // Avoid having random widget focus
  mapWidget->setFocus();
}

// void MainWindow::adjustProfileDockHeight()
// {
//// Load last height
// int profileHeight = Settings::instance().valueInt(lnm::MAINWINDOW_WIDGET_STATE + "ProfileDockHeight", -1);
// if(profileHeight > 20)
// {
//// resizeDocks({ui->dockWidgetProfile}, {profileHeight}, Qt::Vertical);
//// Workaround - set minimum since resize and resize docks does not work
// int oldMinHeight = ui->dockWidgetContentsProfile->minimumHeight();
// ui->dockWidgetContentsProfile->setMinimumHeight(profileHeight);
//// Reset to old value later in event loop
// QTimer::singleShot(0, [ = ]() -> void {
// ui->dockWidgetContentsProfile->setMinimumHeight(oldMinHeight);
// });
// }
// }

/* Enable or disable actions related to online networks */
void MainWindow::updateOnlineActionStates()
{
  if(NavApp::isOnlineNetworkActive())
  {
    // Show action in menu and toolbar
    ui->actionShowAirspacesOnline->setVisible(true);

    // Add tabs in search widget
    if(ui->tabWidgetSearch->indexOf(ui->tabOnlineClientSearch) == -1)
      ui->tabWidgetSearch->addTab(ui->tabOnlineClientSearch, tr("Online Clients"));

    if(ui->tabWidgetSearch->indexOf(ui->tabOnlineCenterSearch) == -1)
      ui->tabWidgetSearch->addTab(ui->tabOnlineCenterSearch, tr("Online Centers"));

    if(ui->tabWidgetSearch->indexOf(ui->tabOnlineServerSearch) == -1)
      ui->tabWidgetSearch->addTab(ui->tabOnlineServerSearch, tr("Online Server"));

    // Add tabs in information widget
    if(ui->tabWidgetInformation->indexOf(ui->tabOnlineClientInfo) == -1)
      ui->tabWidgetInformation->addTab(ui->tabOnlineClientInfo, tr("Online Clients"));

    if(ui->tabWidgetInformation->indexOf(ui->tabOnlineCenterInfo) == -1)
      ui->tabWidgetInformation->addTab(ui->tabOnlineCenterInfo, tr("Online Centers"));
  }
  else
  {
    // Hide action in menu and toolbar
    ui->actionShowAirspacesOnline->setVisible(false);

    // Remove the tabs in search from last to first. Order is important - the search objects remain
    ui->tabWidgetSearch->removeTab(si::SEARCH_ONLINE_SERVER);
    ui->tabWidgetSearch->removeTab(si::SEARCH_ONLINE_CENTER);
    ui->tabWidgetSearch->removeTab(si::SEARCH_ONLINE_CLIENT);

    // Remove tabs in information
    ui->tabWidgetInformation->removeTab(ic::INFO_ONLINE_CENTER);
    ui->tabWidgetInformation->removeTab(ic::INFO_ONLINE_CLIENT);
  }
}

/* Enable or disable actions */
void MainWindow::updateMarkActionStates()
{
  ui->actionMapHideRangeRings->setEnabled(!NavApp::getMapWidget()->getDistanceMarkers().isEmpty() ||
                                          !NavApp::getMapWidget()->getRangeRings().isEmpty() ||
                                          !NavApp::getMapWidget()->getTrafficPatterns().isEmpty());
}

/* Enable or disable actions */
void MainWindow::updateHighlightActionStates()
{
  ui->actionMapClearAllHighlights->setEnabled(
    mapWidget->hasHighlights() || searchController->hasSelection() || routeController->hasSelection());
}

/* Enable or disable actions */
void MainWindow::updateActionStates()
{
  qDebug() << Q_FUNC_INFO;

  if(NavApp::isShuttingDown())
    return;

  ui->actionShowStatusbar->setChecked(!ui->statusBar->isHidden());

  ui->actionClearKml->setEnabled(!mapWidget->getKmlFiles().isEmpty());

  // Enable MORA button depending on available data
  ui->actionMapShowMinimumAltitude->setEnabled(NavApp::getMoraReader()->isDataAvailable());

  bool hasFlightplan = !NavApp::getRouteConst().isFlightplanEmpty();
  ui->actionRouteAppend->setEnabled(hasFlightplan);
  ui->actionRouteSave->setEnabled(hasFlightplan /* && routeController->hasChanged()*/);
  ui->actionRouteSaveAs->setEnabled(hasFlightplan);
  ui->actionRouteSaveAsGfp->setEnabled(hasFlightplan);
  ui->actionRouteSaveAsTxt->setEnabled(hasFlightplan);
  ui->actionRouteSaveAsRte->setEnabled(hasFlightplan);
  ui->actionRouteSaveAsFlp->setEnabled(hasFlightplan);
  ui->actionRouteSaveAsFlightGear->setEnabled(hasFlightplan);
  ui->actionRouteSaveAsFpr->setEnabled(hasFlightplan);
  ui->actionRouteSaveAsFpl->setEnabled(hasFlightplan);
  ui->actionRouteSaveAsCorteIn->setEnabled(hasFlightplan);
  ui->actionRouteSaveAsFms3->setEnabled(hasFlightplan);
  ui->actionRouteSaveAsFms11->setEnabled(hasFlightplan);
  ui->actionRouteSaveAsRxpGtn->setEnabled(hasFlightplan);
  ui->actionRouteSaveAsRxpGns->setEnabled(hasFlightplan);
  ui->actionRouteSaveAsGpx->setEnabled(hasFlightplan);
  ui->actionRouteSaveAsClean->setEnabled(hasFlightplan);

  ui->actionRouteSaveAsIFly->setEnabled(hasFlightplan);
  ui->actionRouteSaveAsXFmc->setEnabled(hasFlightplan);
  ui->actionRouteSaveAsUfmc->setEnabled(hasFlightplan);
  ui->actionRouteSaveAsProSim->setEnabled(hasFlightplan);
  ui->actionRouteSaveAsBbsAirbus->setEnabled(hasFlightplan);
  ui->actionRouteSaveAsFeelthereFpl->setEnabled(hasFlightplan);
  ui->actionRouteSaveAsLeveldRte->setEnabled(hasFlightplan);
  ui->actionRouteSaveAsEfbr->setEnabled(hasFlightplan);
  ui->actionRouteSaveAsQwRte->setEnabled(hasFlightplan);
  ui->actionRouteSaveAsMdx->setEnabled(hasFlightplan);

  ui->actionRouteSaveAsVfp->setEnabled(hasFlightplan);
  ui->actionRouteSaveAsIvap->setEnabled(hasFlightplan);
  ui->actionRouteShowSkyVector->setEnabled(hasFlightplan);

  ui->actionRouteCenter->setEnabled(hasFlightplan);
  ui->actionRouteSelectParking->setEnabled(NavApp::getRouteConst().hasValidDeparture());
  ui->actionMapShowRoute->setEnabled(hasFlightplan);
  ui->actionRouteEditMode->setEnabled(hasFlightplan);
  ui->actionPrintFlightplan->setEnabled(hasFlightplan);
  ui->actionRouteCopyString->setEnabled(hasFlightplan);
  ui->actionRouteAdjustAltitude->setEnabled(hasFlightplan);

  // Remove or add empty airport action from menu and toolbar depending on option
  if(OptionData::instance().getFlags() & opts::MAP_EMPTY_AIRPORTS)
  {
    ui->actionMapShowEmptyAirports->setEnabled(true);

    if(!ui->toolbarMapOptions->actions().contains(ui->actionMapShowEmptyAirports))
    {
      ui->toolbarMapOptions->insertAction(ui->actionMapShowVor, ui->actionMapShowEmptyAirports);
      emptyAirportSeparator = ui->toolbarMapOptions->insertSeparator(ui->actionMapShowVor);
    }

    if(!ui->menuViewAirport->actions().contains(ui->actionMapShowEmptyAirports))
      ui->menuViewAirport->addAction(ui->actionMapShowEmptyAirports);
  }
  else
  {
    ui->actionMapShowEmptyAirports->setDisabled(true);

    if(ui->toolbarMapOptions->actions().contains(ui->actionMapShowEmptyAirports))
    {
      ui->toolbarMapOptions->removeAction(ui->actionMapShowEmptyAirports);

      if(emptyAirportSeparator != nullptr)
        ui->toolbarMapOptions->removeAction(emptyAirportSeparator);
      emptyAirportSeparator = nullptr;
    }

    if(ui->menuViewAirport->actions().contains(ui->actionMapShowEmptyAirports))
      ui->menuViewAirport->removeAction(ui->actionMapShowEmptyAirports);
  }

#ifdef DEBUG_MOVING_AIRPLANE
  ui->actionMapShowAircraft->setEnabled(true);
  ui->actionMapAircraftCenter->setEnabled(true);
  ui->actionMapShowAircraftAi->setEnabled(true);
  ui->actionMapShowAircraftAiBoat->setEnabled(true);
#else
  ui->actionMapAircraftCenter->setEnabled(NavApp::isConnected());
  ui->actionMapShowAircraft->setEnabled(NavApp::isConnected());

  ui->actionMapShowAircraftAi->setEnabled((NavApp::isConnected() && NavApp::isFetchAiAircraft()) ||
                                          NavApp::getOnlinedataController()->isNetworkActive());

  ui->actionMapShowAircraftAiBoat->setEnabled(NavApp::isConnected() && NavApp::isFetchAiShip());
#endif

  ui->actionMapShowAircraftTrack->setEnabled(true);
  ui->actionMapDeleteAircraftTrack->setEnabled(mapWidget->hasTrackPoints() || profileWidget->hasTrackPoints());

  bool canCalcRoute = NavApp::getRouteConst().canCalcRoute();
  ui->actionRouteCalcDirect->setEnabled(canCalcRoute && NavApp::getRouteConst().hasEntries());
  ui->actionRouteCalcRadionav->setEnabled(canCalcRoute);
  ui->actionRouteCalcHighAlt->setEnabled(canCalcRoute);
  ui->actionRouteCalcLowAlt->setEnabled(canCalcRoute);
  ui->actionRouteCalcSetAlt->setEnabled(canCalcRoute && ui->spinBoxRouteAlt->value() > 0);
  ui->actionRouteReverse->setEnabled(canCalcRoute);

  ui->actionMapShowHome->setEnabled(mapWidget->getHomePos().isValid());
  ui->actionMapShowMark->setEnabled(mapWidget->getSearchMarkPos().isValid());

  // Allow to copy airspaces to X-Plane if the currently selected is FSX/P3D and the X-Plane path is set
  ui->actionReloadSceneryCopyAirspaces->setEnabled(
    NavApp::getCurrentSimulatorDb() != atools::fs::FsPaths::XPLANE11 &&
    !NavApp::getSimulatorBasePath(atools::fs::FsPaths::XPLANE11).isEmpty());
}

void MainWindow::resetWindowLayout()
{
  qDebug() << Q_FUNC_INFO;

  setWindowState(windowState() & ~Qt::WindowMinimized);
  setWindowState(windowState() & ~Qt::WindowMaximized);
  setWindowState(windowState() & ~Qt::WindowFullScreen);

  resize(lnm::DEFAULT_MAINWINDOW_SIZE);
  // QRect screenSize = QApplication::desktop()->availableGeometry(this);
  // move((screenSize.width() - lnm::DEFAULT_MAINWINDOW_SIZE.width()) / 2,
  // (screenSize.height() - lnm::DEFAULT_MAINWINDOW_SIZE.height()) / 2);

  restoreState(lnm::DEFAULT_MAINWINDOW_STATE, lnm::MAINWINDOW_STATE_VERSION);

  if(!(OptionData::instance().getFlags2() & opts::MAP_ALLOW_UNDOCK))
    ui->dockWidgetMap->hide();
  else
    ui->dockWidgetMap->show();
}

/* Read settings for all windows, docks, controller and manager classes */
void MainWindow::restoreStateMain()
{
  qDebug() << Q_FUNC_INFO << "enter";

  atools::gui::WidgetState widgetState(lnm::MAINWINDOW_WIDGET);
  widgetState.restore({ui->statusBar, ui->tabWidgetSearch});

  Settings& settings = Settings::instance();

  if(settings.contains(lnm::MAINWINDOW_WIDGET_STATE))
  {
    restoreState(settings.valueVar(lnm::MAINWINDOW_WIDGET_STATE).toByteArray(), lnm::MAINWINDOW_STATE_VERSION);

    move(settings.valueVar(lnm::MAINWINDOW_WIDGET_STATE_POS, QPoint(0, 0)).toPoint());
    resize(settings.valueVar(lnm::MAINWINDOW_WIDGET_STATE_SIZE, lnm::DEFAULT_MAINWINDOW_SIZE).toSize());

#if !defined(Q_OS_MACOS)
    if(settings.valueVar(lnm::MAINWINDOW_WIDGET_STATE_FULLSCREEN, false).toBool())
    {
      qDebug() << Q_FUNC_INFO << "Full screen";
      setWindowState(windowState() & ~Qt::WindowMinimized);
      setWindowState(windowState() & ~Qt::WindowMaximized);
      setWindowState(windowState() | Qt::WindowFullScreen);
    }
    else
#endif
    if(settings.valueVar(lnm::MAINWINDOW_WIDGET_STATE_MAXIMIZED, false).toBool())
    {
      qDebug() << Q_FUNC_INFO << "Maximized";
      setWindowState(windowState() & ~Qt::WindowMinimized);
      setWindowState(windowState() | Qt::WindowMaximized);
      setWindowState(windowState() & ~Qt::WindowFullScreen);
    }
    else
    {
      qDebug() << Q_FUNC_INFO << "Normal";
      setWindowState(windowState() & ~Qt::WindowMinimized);
      setWindowState(windowState() & ~Qt::WindowMaximized);
      setWindowState(windowState() & ~Qt::WindowFullScreen);
    }
  }
  else
    // Use default state saved in application
    resetWindowLayout();

  const QRect geo = QApplication::desktop()->availableGeometry(this);

  // Check if window if off screen
  qDebug() << "Screen geometry" << geo << "Win geometry" << frameGeometry();
  if(!geo.intersects(frameGeometry()))
  {
    qDebug() << Q_FUNC_INFO << "Getting window back on screen";
    move(geo.topLeft());
  }

  // Need to be loaded in constructor first since it reads all options
  // optionsDialog->restoreState();

  qDebug() << "kmlFileHistory";
  kmlFileHistory->restoreState();

  qDebug() << "searchController";
  routeFileHistory->restoreState();

  qDebug() << "searchController";
  searchController->restoreState();

  qDebug() << "mapWidget";
  mapWidget->restoreState();

  qDebug() << "userdataControlle";
  NavApp::getUserdataController()->restoreState();

  qDebug() << "aircraftPerfController";
  NavApp::getAircraftPerfController()->restoreState();

  qDebug() << "routeController";
  routeController->restoreState();

  qDebug() << "profileWidget";
  profileWidget->restoreState();

  qDebug() << "connectClient";
  NavApp::getConnectClient()->restoreState();

  qDebug() << "infoController";
  infoController->restoreState();

  qDebug() << "printSupport";
  printSupport->restoreState();

  widgetState.setBlockSignals(true);
  if(OptionData::instance().getFlags() & opts::STARTUP_LOAD_MAP_SETTINGS)
  {
    // Restore map settings if desired by the user
    widgetState.restore({ui->actionMapShowAirports, ui->actionMapShowSoftAirports,
                         ui->actionMapShowEmptyAirports,
                         ui->actionMapShowAddonAirports,
                         ui->actionMapShowVor, ui->actionMapShowNdb, ui->actionMapShowWp,
                         ui->actionMapShowIls,
                         ui->actionMapShowVictorAirways, ui->actionMapShowJetAirways,
                         ui->actionShowAirspaces, ui->actionShowAirspacesOnline,
                         ui->actionMapShowRoute, ui->actionMapShowAircraft, ui->actionMapShowCompassRose,
                         ui->actionMapAircraftCenter,
                         ui->actionMapShowAircraftAi, ui->actionMapShowAircraftAiBoat,
                         ui->actionMapShowAircraftTrack,
                         ui->actionInfoApproachShowMissedAppr});
  }
  else
    mapWidget->resetSettingActionsToDefault();

  // Map settings that are always loaded
  widgetState.restore({mapProjectionComboBox, mapThemeComboBox, ui->actionMapShowGrid,
                       ui->actionMapShowCities, ui->actionMapShowHillshading, ui->actionRouteEditMode,
                       ui->actionWorkOffline, ui->actionRouteSaveSidStarWaypoints, ui->actionRouteSaveApprWaypoints,
                       ui->actionUserdataCreateLogbook,
                       ui->actionMapShowSunShading, ui->actionMapShowAirportWeather, ui->actionMapShowMinimumAltitude});
  widgetState.setBlockSignals(false);

  firstApplicationStart = settings.valueBool(lnm::MAINWINDOW_FIRSTAPPLICATIONSTART, true);

  // Already loaded in constructor early to allow database creations
  // databaseLoader->restoreState();

  if(!(OptionData::instance().getFlags2() & opts::MAP_ALLOW_UNDOCK))
    ui->dockWidgetMap->hide();
  else
    ui->dockWidgetMap->show();

  qDebug() << Q_FUNC_INFO << "leave";
}

/* Write settings for all windows, docks, controller and manager classes */
void MainWindow::saveStateMain()
{
  qDebug() << "writeSettings";

#ifdef DEBUG_CREATE_WINDOW_STATE
  QStringList hexStr;
  QByteArray state = saveState();
  for(char i : state)
    hexStr.append("0x" + QString::number(static_cast<unsigned char>(i), 16));
  qDebug().noquote().nospace() << "\n\nconst static unsigned char mainWindowState["
                               << state.size() << "] ="
                               << "{" << hexStr.join(",") << "};\n";
#endif

  saveMainWindowStates();

  qDebug() << "searchController";
  if(searchController != nullptr)
    searchController->saveState();

  qDebug() << "mapWidget";
  if(mapWidget != nullptr)
    mapWidget->saveState();

  qDebug() << "userDataController";
  if(NavApp::getUserdataController() != nullptr)
    NavApp::getUserdataController()->saveState();

  qDebug() << "aircraftPerfController";
  if(NavApp::getAircraftPerfController() != nullptr)
    NavApp::getAircraftPerfController()->saveState();

  qDebug() << "routeController";
  if(routeController != nullptr)
    routeController->saveState();

  qDebug() << "profileWidget";
  if(profileWidget != nullptr)
    profileWidget->saveState();

  qDebug() << "connectClient";
  if(NavApp::getConnectClient() != nullptr)
    NavApp::getConnectClient()->saveState();

  qDebug() << "infoController";
  if(infoController != nullptr)
    infoController->saveState();

  saveFileHistoryStates();

  qDebug() << "printSupport";
  if(printSupport != nullptr)
    printSupport->saveState();

  qDebug() << "optionsDialog";
  if(optionsDialog != nullptr)
    optionsDialog->saveState();

  qDebug() << "styleHandler";
  if(NavApp::getStyleHandler() != nullptr)
    NavApp::getStyleHandler()->saveState();

  saveActionStates();

  Settings& settings = Settings::instance();
  settings.setValue(lnm::MAINWINDOW_FIRSTAPPLICATIONSTART, firstApplicationStart);

  qDebug() << "databaseManager";
  if(NavApp::getDatabaseManager() != nullptr)
    NavApp::getDatabaseManager()->saveState();

  qDebug() << "syncSettings";
  settings.syncSettings();
  qDebug() << "save state done";
}

void MainWindow::saveFileHistoryStates()
{
  qDebug() << Q_FUNC_INFO;
  qDebug() << "routeFileHistory";
  if(routeFileHistory != nullptr)
    routeFileHistory->saveState();

  qDebug() << "kmlFileHistory";
  if(kmlFileHistory != nullptr)
    kmlFileHistory->saveState();
  Settings::instance().syncSettings();
}

void MainWindow::saveMainWindowStates()
{
  qDebug() << Q_FUNC_INFO;

  atools::gui::WidgetState widgetState(lnm::MAINWINDOW_WIDGET);
  widgetState.save({ui->statusBar, ui->tabWidgetSearch});

  Settings& settings = Settings::instance();
  settings.setValueVar(lnm::MAINWINDOW_WIDGET_STATE, saveState(lnm::MAINWINDOW_STATE_VERSION));
  settings.setValueVar(lnm::MAINWINDOW_WIDGET_STATE_POS, pos());
  settings.setValueVar(lnm::MAINWINDOW_WIDGET_STATE_SIZE, size());
  settings.setValueVar(lnm::MAINWINDOW_WIDGET_STATE_MAXIMIZED, isMaximized());
  settings.setValueVar(lnm::MAINWINDOW_WIDGET_STATE_FULLSCREEN, isFullScreen());

  qDebug() << Q_FUNC_INFO << "pos" << pos() << "size" << size()
           << "maximized" << isMaximized() << "fullscreen" << isFullScreen();

  // Save profile dock size separately since it is sometimes resized by other docks
  // settings.setValue(lnm::MAINWINDOW_WIDGET_STATE + "ProfileDockHeight", ui->dockWidgetContentsProfile->height());
  Settings::instance().syncSettings();
}

void MainWindow::saveActionStates()
{
  qDebug() << Q_FUNC_INFO;

  atools::gui::WidgetState widgetState(lnm::MAINWINDOW_WIDGET);
  widgetState.save({mapProjectionComboBox, mapThemeComboBox,
                    ui->actionMapShowAirports, ui->actionMapShowSoftAirports, ui->actionMapShowEmptyAirports,
                    ui->actionMapShowAddonAirports,
                    ui->actionMapShowVor, ui->actionMapShowNdb, ui->actionMapShowWp, ui->actionMapShowIls,
                    ui->actionMapShowVictorAirways, ui->actionMapShowJetAirways,
                    ui->actionShowAirspaces, ui->actionShowAirspacesOnline,
                    ui->actionMapShowRoute, ui->actionMapShowAircraft, ui->actionMapShowCompassRose,
                    ui->actionMapAircraftCenter,
                    ui->actionMapShowAircraftAi, ui->actionMapShowAircraftAiBoat,
                    ui->actionMapShowAircraftTrack, ui->actionInfoApproachShowMissedAppr,
                    ui->actionMapShowGrid, ui->actionMapShowCities, ui->actionMapShowSunShading,
                    ui->actionMapShowHillshading, ui->actionMapShowAirportWeather,
                    ui->actionMapShowMinimumAltitude,
                    ui->actionRouteEditMode,
                    ui->actionWorkOffline,
                    ui->actionRouteSaveSidStarWaypoints, ui->actionRouteSaveApprWaypoints,
                    ui->actionUserdataCreateLogbook});
  Settings::instance().syncSettings();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
  // Catch all close events like Ctrl-Q or Menu/Exit or clicking on the
  // close button on the window frame
  qDebug() << Q_FUNC_INFO;

  if(routeController->hasChanged())
  {
    if(!routeCheckForChanges())
    {
      event->ignore();
      return;
    }
  }

  if(NavApp::getAircraftPerfController()->hasChanged())
  {
    if(!NavApp::getAircraftPerfController()->checkForChanges())
    {
      event->ignore();
      return;
    }
  }

  int result = dialog->showQuestionMsgBox(lnm::ACTIONS_SHOW_QUIT,
                                          tr("Really Quit?"),
                                          tr("Do not &show this dialog again."),
                                          QMessageBox::Yes | QMessageBox::No,
                                          QMessageBox::No, QMessageBox::Yes);

  if(result != QMessageBox::Yes)
    event->ignore();

  saveStateMain();
}

void MainWindow::showEvent(QShowEvent *event)
{
  if(firstStart)
  {
    emit windowShown();
    firstStart = false;
  }

  event->ignore();
}

/* Call other other classes to close queries and clear caches */
void MainWindow::preDatabaseLoad()
{
  qDebug() << "MainWindow::preDatabaseLoad";
  if(!hasDatabaseLoadStatus)
  {
    hasDatabaseLoadStatus = true;

    searchController->preDatabaseLoad();
    routeController->preDatabaseLoad();
    mapWidget->preDatabaseLoad();
    profileWidget->preDatabaseLoad();
    infoController->preDatabaseLoad();
    weatherReporter->preDatabaseLoad();

    NavApp::preDatabaseLoad();

    clearWeatherContext();
  }
  else
    qWarning() << "Already in database loading status";
}

/* Call other other classes to reopen queries */
void MainWindow::postDatabaseLoad(atools::fs::FsPaths::SimulatorType type)
{
  qDebug() << "MainWindow::postDatabaseLoad";
  if(hasDatabaseLoadStatus)
  {
    NavApp::postDatabaseLoad();
    searchController->postDatabaseLoad();
    routeController->postDatabaseLoad();
    mapWidget->postDatabaseLoad();
    profileWidget->postDatabaseLoad();
    infoController->postDatabaseLoad();
    weatherReporter->postDatabaseLoad(type);

    // U actions for flight simulator database switch in main menu
    NavApp::getDatabaseManager()->insertSimSwitchActions();

    hasDatabaseLoadStatus = false;
  }
  else
    qWarning() << "Not in database loading status";

  // Database title might have changed
  updateWindowTitle();

  // Update toolbar buttons
  updateActionStates();

  airspaceHandler->updateButtonsAndActions();
}

/* Update the current weather context for the information window. Returns true if any
 * weather has changed or an update is needed */
bool MainWindow::buildWeatherContextForInfo(map::WeatherContext& weatherContext, const map::MapAirport& airport)
{
  opts::Flags flags = OptionData::instance().getFlags();
  opts::Flags2 flags2 = OptionData::instance().getFlags2();
  bool changed = false;
  bool newAirport = currentWeatherContext->ident != airport.ident;

#ifdef DEBUG_INFORMATION_WEATHER
  qDebug() << Q_FUNC_INFO;
#endif

  currentWeatherContext->ident = airport.ident;

  if(flags & opts::WEATHER_INFO_FS)
  {
    if(NavApp::getCurrentSimulatorDb() == atools::fs::FsPaths::XPLANE11)
    {
      currentWeatherContext->fsMetar = weatherReporter->getXplaneMetar(airport.ident, airport.position);
      changed = true;
    }
    else if(NavApp::isConnected())
    {
      // FSX/P3D - Flight simulator fetched weather
      atools::fs::weather::MetarResult metar =
        NavApp::getConnectClient()->requestWeather(airport.ident, airport.position, false /* station only */);

      if(newAirport || (!metar.isEmpty() && metar != currentWeatherContext->fsMetar))
      {
        // Airport has changed or METAR has changed
        currentWeatherContext->fsMetar = metar;
        changed = true;
      }
    }
    else
    {
      if(!currentWeatherContext->fsMetar.isEmpty())
      {
        // If there was a previous metar and the new one is empty we were being disconnected
        currentWeatherContext->fsMetar = atools::fs::weather::MetarResult();
        changed = true;
      }
    }
  }

  if(flags & opts::WEATHER_INFO_ACTIVESKY)
  {
    fillActiveSkyType(*currentWeatherContext, airport.ident);

    QString metarStr = weatherReporter->getActiveSkyMetar(airport.ident);
    if(newAirport || (!metarStr.isEmpty() && metarStr != currentWeatherContext->asMetar))
    {
      // Airport has changed or METAR has changed
      currentWeatherContext->asMetar = metarStr;
      changed = true;
    }
  }

  if(flags & opts::WEATHER_INFO_NOAA)
  {
    atools::fs::weather::MetarResult noaaMetar = weatherReporter->getNoaaMetar(airport.ident, airport.position);
    if(newAirport || (!noaaMetar.isEmpty() && noaaMetar != currentWeatherContext->noaaMetar))
    {
      // Airport has changed or METAR has changed
      currentWeatherContext->noaaMetar = noaaMetar;
      changed = true;
    }
  }

  if(flags & opts::WEATHER_INFO_VATSIM)
  {
    QString metarStr = weatherReporter->getVatsimMetar(airport.ident);
    if(newAirport || (!metarStr.isEmpty() && metarStr != currentWeatherContext->vatsimMetar))
    {
      // Airport has changed or METAR has changed
      currentWeatherContext->vatsimMetar = metarStr;
      changed = true;
    }
  }

  if(flags2 & opts::WEATHER_INFO_IVAO)
  {
    atools::fs::weather::MetarResult ivaoMetar = weatherReporter->getIvaoMetar(airport.ident, airport.position);
    if(newAirport || (!ivaoMetar.isEmpty() && ivaoMetar != currentWeatherContext->ivaoMetar))
    {
      // Airport has changed or METAR has changed
      currentWeatherContext->ivaoMetar = ivaoMetar;
      changed = true;
    }
  }

  weatherContext = *currentWeatherContext;

#ifdef DEBUG_INFORMATION_WEATHER
  if(changed)
    qDebug() << Q_FUNC_INFO << "changed" << changed << weatherContext;
#endif

  return changed;
}

/* Build a normal weather context - used by printing */
void MainWindow::buildWeatherContext(map::WeatherContext& weatherContext, const map::MapAirport& airport) const
{
  opts::Flags flags = OptionData::instance().getFlags();
  opts::Flags2 flags2 = OptionData::instance().getFlags2();

  weatherContext.ident = airport.ident;

  if(flags & opts::WEATHER_INFO_FS)
  {
    if(NavApp::getCurrentSimulatorDb() == atools::fs::FsPaths::XPLANE11)
      weatherContext.fsMetar = weatherReporter->getXplaneMetar(airport.ident, airport.position);
    else
      weatherContext.fsMetar =
        NavApp::getConnectClient()->requestWeather(airport.ident, airport.position, false /* station only */);
  }

  if(flags & opts::WEATHER_INFO_ACTIVESKY)
  {
    weatherContext.asMetar = weatherReporter->getActiveSkyMetar(airport.ident);
    fillActiveSkyType(weatherContext, airport.ident);
  }

  if(flags & opts::WEATHER_INFO_NOAA)
    weatherContext.noaaMetar = weatherReporter->getNoaaMetar(airport.ident, airport.position);

  if(flags & opts::WEATHER_INFO_VATSIM)
    weatherContext.vatsimMetar = weatherReporter->getVatsimMetar(airport.ident);

  if(flags2 & opts::WEATHER_INFO_IVAO)
    weatherContext.ivaoMetar = weatherReporter->getIvaoMetar(airport.ident, airport.position);
}

/* Build a temporary weather context for the map tooltip */
void MainWindow::buildWeatherContextForTooltip(map::WeatherContext& weatherContext,
                                               const map::MapAirport& airport) const
{
  opts::Flags flags = OptionData::instance().getFlags();

  weatherContext.ident = airport.ident;

  if(flags & opts::WEATHER_TOOLTIP_FS)
  {
    if(NavApp::getCurrentSimulatorDb() == atools::fs::FsPaths::XPLANE11)
      weatherContext.fsMetar = weatherReporter->getXplaneMetar(airport.ident, airport.position);
    else
      weatherContext.fsMetar =
        NavApp::getConnectClient()->requestWeather(airport.ident, airport.position, false /* station only */);
  }

  if(flags & opts::WEATHER_TOOLTIP_ACTIVESKY)
  {
    weatherContext.asMetar = weatherReporter->getActiveSkyMetar(airport.ident);
    fillActiveSkyType(weatherContext, airport.ident);
  }

  if(flags & opts::WEATHER_TOOLTIP_NOAA)
    weatherContext.noaaMetar = weatherReporter->getNoaaMetar(airport.ident, airport.position);

  if(flags & opts::WEATHER_TOOLTIP_VATSIM)
    weatherContext.vatsimMetar = weatherReporter->getVatsimMetar(airport.ident);

  opts::Flags2 flags2 = OptionData::instance().getFlags2();
  if(flags2 & opts::WEATHER_TOOLTIP_IVAO)
    weatherContext.ivaoMetar = weatherReporter->getIvaoMetar(airport.ident, airport.position);
}

/* Fill active sky information into the weather context */
void MainWindow::fillActiveSkyType(map::WeatherContext& weatherContext, const QString& airportIdent) const
{
  weatherContext.asType = weatherReporter->getCurrentActiveSkyName();
  weatherContext.isAsDeparture = false;
  weatherContext.isAsDestination = false;

  if(weatherReporter->getActiveSkyDepartureIdent() == airportIdent)
    weatherContext.isAsDeparture = true;
  if(weatherReporter->getActiveSkyDestinationIdent() == airportIdent)
    weatherContext.isAsDestination = true;
}

void MainWindow::clearWeatherContext()
{
  qDebug() << Q_FUNC_INFO;

  // Clear all weather and fetch new
  *currentWeatherContext = map::WeatherContext();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
  qDebug() << Q_FUNC_INFO;
  // Accept only one flight plan
  if(event->mimeData()->urls().size() == 1)
  {
    // Has to be a file
    QUrl url = event->mimeData()->urls().first();
    if(url.isLocalFile())
    {
      // accept if file exists, is readable and matches the supported extensions
      QFileInfo file(url.toLocalFile());
      if(file.exists() && file.isReadable() && file.isFile() &&
         (atools::fs::pln::FlightplanIO::detectFormat(file.filePath()) ||
          AircraftPerfController::isPerformanceFile(file.filePath())))
      {

        qDebug() << Q_FUNC_INFO << "accepting" << url;
        event->acceptProposedAction();
        return;
      }
    }
    qDebug() << Q_FUNC_INFO << "not accepting" << url;
  }
}

void MainWindow::dropEvent(QDropEvent *event)
{
  if(event->mimeData()->urls().size() == 1)
  {
    QString filepath = event->mimeData()->urls().first().toLocalFile();
    qDebug() << Q_FUNC_INFO << "Dropped file:" << filepath;

    if(AircraftPerfController::isPerformanceFile(filepath))
      // Load aircraft performance file
      NavApp::getAircraftPerfController()->loadFile(filepath);
    else
      // Load flight plan
      routeOpenFile(filepath);
  }
}

void MainWindow::updateErrorLabels()
{
  QString tooltip;
  QString err;
  // Show only if route is valid, there are errors and nothing is collecting performance
  bool showError = NavApp::getRoute().size() >= 2 && NavApp::getAltitudeLegs().hasErrors() &&
                   !NavApp::isCollectingPerformance();
  if(showError)
    err = atools::util::HtmlBuilder::errorMessage(NavApp::getAltitudeLegs().getErrorStrings(tooltip).join(" "));

  ui->labelRouteError->setVisible(showError);
  ui->labelRouteError->setText(err);
  ui->labelRouteError->setToolTip(tooltip);
  ui->labelRouteError->setStatusTip(tooltip);

  ui->labelProfileError->setVisible(showError);
  ui->labelProfileError->setText(err);
  ui->labelProfileError->setToolTip(tooltip);
  ui->labelProfileError->setStatusTip(tooltip);
}

map::MapThemeComboIndex MainWindow::getMapThemeIndex() const
{
  return static_cast<map::MapThemeComboIndex>(mapThemeComboBox->currentIndex());
}
