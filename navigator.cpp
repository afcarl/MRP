/*
 * navigator.cpp
 *
 *  Created on: May 17, 2012
 *      Author: derrick
 */

#include "sensor/sonarmodel.h"
#include "mapping/dynamicmap.h"
#include "localization/localparticle.h"
#include "nav/roadmap.h"
#include "apf/mindistcalc.h"
#include "visualization/viswindow.h"

#include <pthread.h>
#include <unistd.h>
#include <fstream>

#include <cmath>

using namespace std;
using namespace PlayerCc;

#define USE_VIS 1

/*
 * The number of particles to use for localization.
 *
 * BM NumParts
 */
#define NUM_PARTICLES 1000

#define PARTICLE_THRESH 0.02

#define SONAR_RES 0.3

#define RAND_SPAWN 0.2
#define POS_JITTER 0.2
#define YAW_JITTER 0.2

#define MATCH 3.0
#define MISMATCH 0.2

//If V goes above start value, start planning paths.
#define PATH_START 0.4

//If V then drops below stop value, wait to relocalize.
#define PATH_STOP 2

/*
 * The minimal arrival distance for intermediate points generated by the path
 * planning algorithm. This can be significantly larger than WAYPT_THRESH since
 * these will not be mission critical points.
 */
#define NAV_THRESH 1.0

/*
 * The threshhold for waypoint arrival in meters. Defined by the project
 * assignment.
 */
#define WAYPT_THRESH 0.25

#define MAX_VEL 0.4

#define APF_MIN_D 0.65
#define APF_FACT 4

#define KGOAL 1.8
#define KOBS 0.5

/**
 * Flag for signalling that the program is ready to run
 */
volatile bool ready = false;

/**
 * Tells apf to read the localNavPt variable for it's current nav goal in robot-
 * local space. Used during the initial wander to localize the kidnapped robot.
 */
volatile bool doPath = false;

/**
 * The navigation point in robot local space to be used when navLocal is true.
 */
Pos2 localNavPt;

/**
 * The pose of the robot based purely on the odometry.
 */
Pose odomToRobot;

/**
 * The offset of the odometry origin in the map.
 */
Pose mapToOdom;

Pose robot;

/**
 * The current goal point in map space for APF.
 */
Pos2 curGoal;

/**
 * The current index of the waypoint being navigated to (once localized).
 */
int curWaypt = 0;

/**
 * The std deviation in the localization particles.
 */
volatile double localValue = -1;

/**
 * The list of waypoints to navigate to once localized.
 */
Pos2List waypoints;

DynamicMap *pMap;

static PlayerClient *pRobot;
static Position2dProxy *pPosition;
static SonarProxy *pSonar;

pthread_t thrNav;
pthread_t thrLocal;
pthread_t thrDrive;
pthread_t thrMission;
pthread_t thrVis;
pthread_t thrPose;

pthread_mutex_t localUpdate = PTHREAD_MUTEX_INITIALIZER;

SensorSonar *sSonar;
SensorModel *sonar;

Roadmap *rMap;

MinDistCalc *apf_calc;
APF *apf;

double navThresh;

Pos2 drvCmd;

Path *curPath;

ParticleLocalization *pLocal;

VisImage *im;

bool drawing = false;
volatile bool localizing = false;
volatile bool gettingPose = false;
volatile bool sensorUpdate = false;

PoseV estPose;

double cDir[] = { 0.7, 0.7, 0.0 };
double cOdom[] = { 0.0, 0.9, 0.0 };
double cLoc[] = { 0.0, 0.7, 0.7 };
double cPt[] = { 0.8, 0.0, 0.0 };
double cPath[] = { 0.9, 0.0, 0.9 };

double W, H, w, h, l, t;

/**
 * Simple function used to block threads until the ready flag is set.
 */
void waitReady() {
  while (!ready) {
    sleep(0);
  }
}

inline void drawPoint(VisImage *im, size_t x, size_t y, size_t s, double *c) {
  for (size_t xi = x - s; xi <= x + s; ++xi) {
    for (size_t yi = y - s; yi <= y + s; ++yi) {
      VisSetPx(im, xi, yi, c);
    }
  }
}

inline void drawPos2(VisImage *im, const double& x, const double& y, size_t s,
    double *c) {
  size_t xi = (x - l) / W;
  size_t yi = (t - y) / H;
  drawPoint(im, xi, yi, s, c);
}

inline void drawPos2(VisImage *im, const Pos2& p, size_t s, double *c) {
  drawPos2(im, p.x, p.y, s, c);
}

inline void drawPose(VisImage *im, const Pose& p, size_t s, double *c) {
  drawPos2(im, p.p, s, c);

  double x = p.p.x + cos(p.yaw) / 2.0;
  double y = p.p.y + sin(p.yaw) / 2.0;
  drawPos2(im, x, y, s, cDir);
}

void visRefresh() {
  //BM visLoop
  drawing = true;
  w = pMap->envWidth();
  h = pMap->envHeight();

  //cout << w << ", " << h << endl;

  W = w / im->width;
  H = h / im->height;

  //Keep the pixel aspect correct
  W = W > H ? W : H;
  H = H > W ? H : W;

  l = pMap->envLeft() + pMap->gridRes() / 2;
  t = pMap->envTop() - pMap->gridRes() / 2;

  size_t idx = 0;

  bool done = false;

  double px[3];

  //Draw the map first
  for (size_t xi = 0; xi < im->width && !done; ++xi) {
    double x = (xi * W) + l;

    for (size_t yi = 0; yi < im->height; ++yi) {
      double y = t - yi * H;

      double c = 1 - OtoP(pMap->get(x, y));

      //std::cout << "\t(" << x << ", " << y << ", " << c << ")" << std::endl;

      px[0] = c;
      px[1] = c;
      px[2] = c;
      VisSetPx(im, xi, yi, px);
    }
  }

  PoseVList *pts = pLocal->getParticles();

  for (PoseVList::iterator i = pts->begin(); i != pts->end(); ++i) {
    drawPose(im, i->pose, 0, cPt);
  }

  drawPose(im, estPose.pose, 1, cLoc);

  drawPose(im, odomToRobot, 1, cOdom);

  if (curPath && doPath) {
    Pos2List *pts = curPath->getPoints();
    for (Pos2List::iterator i = pts->begin(); i != pts->end(); ++i) {
      drawPos2(im, *i, 1, cPath);
    }
  }

  //cout << estPose.v << " " << flush; //(" << estPose.pose.p.x << ", " << estPose.pose.p.y
  //    << ") (" << robot.p.x << ", " << robot.p.y << ")" << endl;

  drawing = false;
}

/**
 * Handles the overall state control for the robot.
 * @param arg
 */
void * missionLoop(void *arg) {
  //BM missonLoop

  drvCmd.x = 0;
  drvCmd.y = 0;

  waitReady();

  while (curWaypt < waypoints.size()) {
    usleep(100);

    if (doPath && estPose.v > PATH_STOP) {
      doPath = false;
      localNavPt.x = 0;
      localNavPt.y = 0;
    } else if (!doPath && estPose.v < PATH_START) {
      cout << "Enabling path mode." << endl;
      pPosition->SetSpeed(0, 0);
      doPath = true;
    } else if (!doPath) {
      //Drive forward with a slight tendency to turn. Sort of wall follows,
      //but has momentum to bounce past hallways. All robots seem to travel far
      //enough to localize though.
      localNavPt.x = 0.5;
      localNavPt.y = -0.2;
    }
  }

  cout << "Reached last point, queue up \"We Are the Champions\"!" << endl;

  //The threads do not degrade gracefully, so don't give them a chance to screw
  //up the win!
  exit(0);
  return 0;
}

/**
 * Thread loop for running APF and Goto
 *
 * @param arg
 */
void * driveLoop(void *arg) {

  waitReady();
  double v, t;
  Pos2 localGoal, drv, localFix;

  Pos2List obslist;
  Pose dPose, prevPose, localPose;

  while (true) {
    try {
      pRobot->Read();

      odomToRobot.p.x = pPosition->GetXPos();
      odomToRobot.p.y = pPosition->GetYPos();
      odomToRobot.yaw = pPosition->GetYaw();

      robot = mapToOdom + odomToRobot;

      pPosition->SetSpeed(drvCmd.x, drvCmd.y);

    } catch (PlayerError& e) {
      cerr << e << endl;
      exit(1);
    }

    dPose = odomToRobot - prevPose;
    prevPose = odomToRobot;

    pthread_mutex_lock(&localUpdate);
    pLocal->motionUpdate(dPose);
    pthread_mutex_unlock(&localUpdate);

    //BM Driveloop

    if (!doPath) {
      //Move point into odom space
      localGoal = rotate(localNavPt, odomToRobot.yaw);
      localGoal.x += odomToRobot.p.x;
      localGoal.y += odomToRobot.p.y;
      localPose = odomToRobot;
    } else {
      localPose = estPose.pose;
      curPath->getCurrentPt(localGoal);

      //cout << "\tHeading to point #" << curPath->curIndex() << endl;
      //cout << "\t\t(" << localGoal.x << ", " << localGoal.y << ")" << endl;
    }

    obslist = sSonar->getLocalScan(dtor(90), dtor(-90));
    drv = apf->calc(localPose, localGoal, obslist);

    v = drv.x;

    if (v < 0)
      v = 0;
    else if (v > MAX_VEL)
      v = MAX_VEL;

    t = atan2(drv.y, drv.x) / 4;

    if (doPath) {
      //Prevent the robot from toilet-bowling around a close waypoint to it's side
      double c = cos(t);
      v *= c * navThresh;
    }

    //cout << "Vel: " << v << " Turn: " << t << endl;

    drvCmd.x = v;
    drvCmd.y = t;
  }

  return 0;
}

/**
 * Thread loop for running localization. This is the loop that controls the
 * localized flag.
 *
 * @param arg
 */
void * localLoop(void *arg) {

  Pose prevPose, dPose;
  prevPose.p.x = 0;
  prevPose.p.y = 0;
  prevPose.yaw = 0;

  waitReady();

  //BM Localization loop

  while (true) {
    usleep(0);

    if (gettingPose) {
      continue;
    }

    if (!pthread_mutex_trylock(&localUpdate)) {
      pLocal->sensorUpdate();
      pthread_mutex_unlock(&localUpdate);
    }

    //estPose = pLocal->getPose();

    if (USE_VIS) {
      visRefresh();
    }

    //localError = est.v;
  }

  return 0;
}

/**
 * BM poseLoop
 *
 * @param arg
 */
void * poseLoop(void *arg) {

  PoseV lastPose;

  waitReady();

  while (true) {
    usleep(10000);
    if (pthread_mutex_trylock(&localUpdate)) {
      continue;
    }

    gettingPose = true;
    lastPose = estPose;
    estPose = pLocal->getPose();

    pthread_mutex_unlock(&localUpdate);

    //Check if the localization point moved from cluster to cluster within a
    //single iteration and didn't otherwise knock the robot out of pathing.
    if (doPath && pointDist(lastPose.pose.p, estPose.pose.p) > 3.0) {
      cout << "Pose estimate delta too large, resetting path mode." << endl;
      doPath = false;
    }

    gettingPose = false;
    if (estPose.v > PATH_START) {
      mapToOdom = estPose.pose - odomToRobot;
    }
  }

  return 0;
}

/**
 * Thread loop for performing path planning. Running this loop is controlled by
 * the localized flag.
 *
 * BM navLoop
 *
 * @param arg A Pos2List pointer to the list of points to navigate to.
 */
void * navLoop(void *arg) {

  /**
   * The last waypoint that a path was generated to. When curGoal is greater
   * than this number, a new path must be generated.
   */
  int lastGen = -1;

  bool lastDoPath = doPath;

  waitReady();

  while (true) {
    usleep(1000);

    //If localization drops out, reset lastGen so that we regenerate a path
    //to the next waypoint once we're localized again.
    if (lastDoPath && !doPath) {
      cout << "\nLost path flag. Waiting for re-localization." << endl;
      --lastGen;
    }

    lastDoPath = doPath;

    if (!doPath) {
      continue;
    }

    if (lastGen < curWaypt) {
      //cout << "Generating path..." << endl;
      //Need to generate a path
      curPath = rMap->getPath(estPose.pose.p, waypoints[curWaypt]);
      if (!curPath) {
        cout << "\nNo path returned. Returning to localization mode." << endl;
        doPath = false;
        continue;
      }
      ++lastGen;

      /*for (Pos2List::iterator i = curPath->getPoints()->begin();
          i != curPath->getPoints()->end(); ++i) {
        cout << "(" << i->x << ", " << i->y << ") ";
      }

      cout << endl;*/

      curPath->getCurrentPt(curGoal);
    }

    char navMsg[] = "\nReached intermediate navigation point.";
    char wayptMsg[] = "\nReached assigned goal waypoint.";
    char *msg;

    if (curPath->curIndex() == curPath->numPts() - 1) {
      //Final point in a path is an actual assigned goal point
      navThresh = WAYPT_THRESH;
      msg = wayptMsg;
    } else {
      navThresh = NAV_THRESH;
      msg = navMsg;
    }

    if (curPath->update(estPose.pose, navThresh)) {
      cout << msg << endl;
      curPath->getCurrentPt(curGoal);

      if (curPath->atEnd()) {
        ++curWaypt;
      }
    }
  }

  return 0;
}

/**
 * Reads in waypoints from a file
 *
 * @param filename The file containing the x,y pairs of the points.
 * @param pts The list to put the points read in.
 * @return The number of points read from the file.
 */
size_t readPoints(std::string filename, Pos2List& pts) {
  Pos2 pt;
  size_t n = 0;

  std::ifstream in(filename.c_str(), std::ifstream::in);

  while (in.good()) {
    in >> pt.x;
    in >> pt.y;
    if (in.good()) {
      pts.push_back(pt);
      ++n;
    }
  }

  in.close();

  return n;
}

void usage(char *ex) {
  cerr << endl << "Usage: " << ex << " [Host] [Port] <Waypoints> [Map] " << endl
      << endl;

  cerr << "Host- The host (ip or name) where the player server is running."
      << endl << "\tOptional, assumed to be localhost if not specified." << endl
      << endl;

  cerr << "Port- The port number of the robot to connect to." << endl
      << "\tOptional, assumed to be 6665 if not specified." << endl << endl;

  cerr
      << "Waypoints- The file containing a set of x,y coordinates to navigate to,"
      << endl << "\tin order." << endl << endl;

  cerr << "Map- The map file to use for the navigation. The above waypoints are"
      << endl << "\texpected to follow the coordinate system setup by this map."
      << endl << endl;
}

void threadError(int err, std::string name) {
  cerr << "Error starting " << name << " thread." << endl << "\t";

  switch (err) {
  case EAGAIN:
    cerr << "Insufficient system resources.";
    break;

  case EINVAL:
    cerr << "Invalid attr parameter.";
    break;

  case EPERM:
    cerr << "Invalid permissions.";
    break;
  }

  cerr << endl;
  exit(1);
}

void spawnThreads() {
  //BM spawnThreads

  cout << "Spawning threads..." << flush;
  int ret;

  ret = pthread_create(&thrDrive, 0, driveLoop, 0);
  if (ret) {
    threadError(ret, "drive");
  }

  ret = pthread_create(&thrNav, 0, navLoop, 0);
  if (ret) {
    threadError(ret, "path");
  }

  ret = pthread_create(&thrLocal, 0, localLoop, 0);
  if (ret) {
    threadError(ret, "localization");
  }

  ret = pthread_create(&thrMission, 0, missionLoop, 0);
  if (ret) {
    threadError(ret, "mission");
  }

  ret = pthread_create(&thrPose, 0, poseLoop, 0);
  if (ret) {
    threadError(ret, "pose");
  }

  /*ret = pthread_create(&thrVis, 0, visLoop, 0);
   if (ret) {
   threadError(ret, "visualization");
   }*/

  cout << "Done" << endl;
}

bool processArgs(int argc, char**argv, std::string& host, int& port,
    std::string& points, std::string& map) {

  //BM processArgs

  std::string defHost("localhost");
  int defPort = 6665;

  host = defHost;
  port = defPort;

  if (argc < 2 || argc > 5) {
    return true;
  }

  int wayptI = 1;
  int mapI = 2;

  if (argc == 5) {
    host = argv[1];
    port = atoi(argv[2]);

    wayptI += 2;
    mapI += 2;
  } else if (argc == 4) {
    host = argv[1];
    if (host.find('.') == string::npos) {
      //No periods in argument, see if it's a port
      port = atoi(argv[1]);
      if (port == 0) {
        //wasn't actually a port, must be a hostname
        port = defPort;
      } else {
        //Seems to be a port, reset host to defHost
        host = defHost;
      }
    }

    ++wayptI;
    ++mapI;
  }

  points = argv[wayptI];
  map = argv[mapI];

  cout << "Arguments:" << endl;
  cout << "\tWaypoints - " << points << endl;
  cout << "\tMap - " << map << endl;
  cout << "\tHost - " << host << endl;
  cout << "\tPort - " << port << endl;

  /*for (int i = 0; i < argc; ++i) {
   cout << "\t" << i << " - " << argv[i] << endl;
   }*/

  return false;
}

void initProxies(std::string host, int port) {
  cout << "Initializing Player/Stage proxies..." << flush;

  pRobot = new PlayerClient(host, port);
  pPosition = new Position2dProxy(pRobot, 0);
  pSonar = new SonarProxy(pRobot, 0);

  pPosition->SetMotorEnable(true);

  cout << "Done" << endl;
}

void initSensor() {
  cout << "Initializing sensor and model..." << flush;

  sSonar = new SensorSonar(pSonar, &Pioneer8Sonar[0], 8, 0, 5.0);

  SonarLocalProfile *sProf = new SonarLocalProfile(MATCH, MISMATCH, SONAR_RES);
  SonarIterativeRegion *sReg = new SonarIterativeRegion(1, 0.05, 0, SONAR_RES);
  sonar = new SensorModel(sSonar, sReg, sProf, PtoO(.7));

  cout << "Done" << endl;
}

void loadMap(std::string mapfile) {
  cout << "Loading map..." << flush;

  pMap = DynamicMap::loadMap(mapfile, 10.0, 10.0);

  cout << "Done" << endl;
}

void loadRoadmap(std::string filename) {
  cout << "Loading roadmap..." << flush;

  rMap = new Roadmap();
  if (rMap->loadFromFile(filename)) {
    cerr << "Error loading roadmap." << endl;
    exit(1);
  }

  cout << "Done" << endl;
}

void initAPF() {
  cout << "Initializing apf..." << flush;
  apf_calc = new MinDistCalc(APF_MIN_D, APF_FACT);
  apf = new APF(KGOAL, KOBS, apf_calc);
  cout << "Done" << endl;
}

void initLocal() {
  //BM Localization init

  cout << "Initializing particle localization..." << flush;
  pLocal = new ParticleLocalization(sonar, pMap, NUM_PARTICLES, PARTICLE_THRESH,
      0.3, RAND_SPAWN, POS_JITTER, YAW_JITTER);
  cout << "Done" << endl;
}

void initVis() {
  cout << "Initializing visualization window..." << flush;
  im = initVis(1000, 450, true, &drawing);
  cout << "Done" << endl;
}

int main(int argc, char** argv) {

  std::string ptfile, mapfile, host, roadmapfile = "maps/gccis3.roadmap";
  int port;

  srand(time(0));

  //Proccess the args and ensure we have everything we need
  if (processArgs(argc, argv, host, port, ptfile, mapfile)) {
    usage(argv[0]);
    exit(1);
  }

  //Read in the points from the waypoint file
  if (!readPoints(ptfile, waypoints)) {
    cerr << "No waypoints read from file. Ensure that the file exists." << endl;
    exit(1);
  }
  cout << endl << "Read in " << waypoints.size() << " points." << endl;

  //Init the rest of the various parts of the system
  initProxies(host, port);
  initSensor();
  initAPF();
  loadMap(mapfile);
  loadRoadmap(roadmapfile);
  initLocal();

  if (USE_VIS) {
    initVis();
  }

  spawnThreads();

  //Tell the threads that they can start

  while (pSonar->GetCount() < 8) {
    pRobot->Read();
  }

  ready = true;

  /*
   * We only need to join the nav loop. The nav loop will end once the last
   * waypoint has been hit.
   */
  pthread_join(thrMission, 0);

  return 0;
}
