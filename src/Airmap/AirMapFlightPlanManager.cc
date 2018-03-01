/****************************************************************************
 *
 *   (c) 2009-2016 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "AirMapFlightPlanManager.h"
#include "AirMapManager.h"
#include "AirMapRulesetsManager.h"
#include "AirMapAdvisoryManager.h"
#include "QGCApplication.h"
#include "SettingsManager.h"

#include "PlanMasterController.h"
#include "QGCMAVLink.h"

#include "airmap/pilots.h"
#include "airmap/flights.h"
#include "airmap/date_time.h"
#include "airmap/flight_plans.h"
#include "airmap/geometry.h"

using namespace airmap;

//-----------------------------------------------------------------------------
AirMapFlightAuthorization::AirMapFlightAuthorization(const Evaluation::Authorization auth, QObject *parent)
    : AirspaceFlightAuthorization(parent)
    , _auth(auth)
{
}

//-----------------------------------------------------------------------------
AirspaceFlightAuthorization::AuthorizationStatus
AirMapFlightAuthorization::status()
{
    switch(_auth.status) {
    case Evaluation::Authorization::Status::accepted:
        return AirspaceFlightAuthorization::Accepted;
    case Evaluation::Authorization::Status::rejected:
        return AirspaceFlightAuthorization::Rejected;
    case Evaluation::Authorization::Status::pending:
        return AirspaceFlightAuthorization::Pending;
    case Evaluation::Authorization::Status::accepted_upon_submission:
        return AirspaceFlightAuthorization::AcceptedOnSubmission;
    case Evaluation::Authorization::Status::rejected_upon_submission:
        return AirspaceFlightAuthorization::RejectedOnSubmission;
    }
    return AirspaceFlightAuthorization::Unknown;
}

//-----------------------------------------------------------------------------
AirMapFlightInfo::AirMapFlightInfo(const airmap::Flight& flight, QObject *parent)
    : AirspaceFlightInfo(parent)
    , _flight(flight)
{
    //-- TODO: Load bounding box geometry


}

//-----------------------------------------------------------------------------
QString
AirMapFlightInfo::createdTime()
{
    return QDateTime::fromMSecsSinceEpoch((quint64)airmap::milliseconds_since_epoch(_flight.created_at)).toString("yyyy MM dd - hh:mm:ss");
}

//-----------------------------------------------------------------------------
QString
AirMapFlightInfo::startTime()
{
    return QDateTime::fromMSecsSinceEpoch((quint64)airmap::milliseconds_since_epoch(_flight.start_time)).toString("yyyy MM dd - hh:mm:ss");
}

//-----------------------------------------------------------------------------
QString
AirMapFlightInfo::endTime()
{
    return QDateTime::fromMSecsSinceEpoch((quint64)airmap::milliseconds_since_epoch(_flight.end_time)).toString("yyyy MM dd - hh:mm:ss");
}

//-----------------------------------------------------------------------------
AirMapFlightPlanManager::AirMapFlightPlanManager(AirMapSharedState& shared, QObject *parent)
    : AirspaceFlightPlanProvider(parent)
    , _shared(shared)
{
    connect(&_pollTimer, &QTimer::timeout, this, &AirMapFlightPlanManager::_pollBriefing);
    //-- Set some valid, initial start/end time
    _flightStartTime = QDateTime::currentDateTime().addSecs(10 * 60);
    _flightEndTime   = _flightStartTime.addSecs(30 * 60);
}

//-----------------------------------------------------------------------------
AirMapFlightPlanManager::~AirMapFlightPlanManager()
{
    _advisories.deleteListAndContents();
    _rulesets.deleteListAndContents();
}

//-----------------------------------------------------------------------------
void
AirMapFlightPlanManager::setFlightStartTime(QDateTime start)
{
    if(_flightStartTime != start) {
        //-- Can't start in the past
        if(start < QDateTime::currentDateTime()) {
            start = QDateTime::currentDateTime().addSecs(5 * 60);
        }
        _flightStartTime = start;
        emit flightStartTimeChanged();
    }
}

//-----------------------------------------------------------------------------
void
AirMapFlightPlanManager::setFlightEndTime(QDateTime end)
{
    if(_flightEndTime != end) {
        //-- End has to be after start
        if(end < _flightStartTime) {
            end = _flightStartTime.addSecs(30 * 60);
        }
        _flightEndTime = end;
        emit flightEndTimeChanged();
    }
}

//-----------------------------------------------------------------------------
void
AirMapFlightPlanManager::startFlightPlanning(PlanMasterController *planController)
{
    if (!_shared.client()) {
        qCDebug(AirMapManagerLog) << "No AirMap client instance. Will not create a flight";
        return;
    }

    if (_state != State::Idle) {
        qCWarning(AirMapManagerLog) << "AirMapFlightPlanManager::startFlightPlanning: State not idle";
        return;
    }

    //-- TODO: Check if there is an ongoing flight plan and do something about it (Delete it?)

    /*
     * if(!_flightPlan.isEmpty()) {
     *     do something;
     * }
     */

    if(!_planController) {
        _planController = planController;
        //-- Get notified of mission changes
        connect(planController->missionController(), &MissionController::missionBoundingCubeChanged, this, &AirMapFlightPlanManager::_missionChanged);
    }
}

//-----------------------------------------------------------------------------
void
AirMapFlightPlanManager::submitFlightPlan()
{
    if(_flightPlan.isEmpty()) {
        qCWarning(AirMapManagerLog) << "Submit flight with no flight plan.";
        return;
    }
    _flightId.clear();
    _state = State::FlightSubmit;
    FlightPlans::Submit::Parameters params;
    params.authorization = _shared.loginToken().toStdString();
    params.id            = _flightPlan.toStdString();
    std::weak_ptr<LifetimeChecker> isAlive(_instance);
    _shared.client()->flight_plans().submit(params, [this, isAlive](const FlightPlans::Submit::Result& result) {
        if (!isAlive.lock()) return;
        if (_state != State::FlightSubmit) return;
        if (result) {
            _flightId = QString::fromStdString(result.value().flight_id.get());
            _state    = State::FlightPolling;
            _pollBriefing();
        } else {
            QString description = QString::fromStdString(result.error().description() ? result.error().description().get() : "");
            emit error("Failed to submit Flight Plan",
                    QString::fromStdString(result.error().message()), description);
            _state = State::Idle;
            _flightPermitStatus = AirspaceFlightPlanProvider::PermitRejected;
            emit flightPermitStatusChanged();
        }
    });
}

//-----------------------------------------------------------------------------
void
AirMapFlightPlanManager::updateFlightPlan()
{
    //-- Are we enabled?
    if(!qgcApp()->toolbox()->settingsManager()->airMapSettings()->enableAirMap()->rawValue().toBool()) {
        return;
    }
    //-- Do we have a license?
    if(!_shared.hasAPIKey()) {
        return;
    }
    _flightPermitStatus = AirspaceFlightPlanProvider::PermitPending;
    emit flightPermitStatusChanged();
    _updateFlightPlan();
}

//-----------------------------------------------------------------------------
void
AirMapFlightPlanManager::deleteSelectedFlightPlans()
{
    qCDebug(AirMapManagerLog) << "Delete flight plan";
    _flightsToDelete.clear();
    for(int i = 0; i < _flightList.count(); i++) {
        AirspaceFlightInfo* pInfo = _flightList.get(i);
        if(pInfo && pInfo->selected()) {
            _flightsToDelete << pInfo->flightPlanID();
        }
    }
    if(_flightsToDelete.count()) {
        deleteFlightPlan(QString());
    }
}

//-----------------------------------------------------------------------------
void
AirMapFlightPlanManager::deleteFlightPlan(QString flightPlanID)
{
    qCDebug(AirMapManagerLog) << "Delete flight plan";
    if(!flightPlanID.isEmpty()) {
        _flightsToDelete.clear();
        _flightsToDelete << flightPlanID;
    }
    if (_pilotID == "") {
        //-- Need to get the pilot id
        qCDebug(AirMapManagerLog) << "Getting pilot ID";
        _state = State::GetPilotID;
        std::weak_ptr<LifetimeChecker> isAlive(_instance);
        _shared.doRequestWithLogin([this, isAlive](const QString& login_token) {
            if (!isAlive.lock()) return;
            Pilots::Authenticated::Parameters params;
            params.authorization = login_token.toStdString();
            _shared.client()->pilots().authenticated(params, [this, isAlive](const Pilots::Authenticated::Result& result) {
                if (!isAlive.lock()) return;
                if (_state != State::GetPilotID) return;
                if (result) {
                    _pilotID = QString::fromStdString(result.value().id);
                    qCDebug(AirMapManagerLog) << "Got Pilot ID:"<<_pilotID;
                    _state = State::Idle;
                    _deleteFlightPlan();
                } else {
                    _state = State::Idle;
                    QString description = QString::fromStdString(result.error().description() ? result.error().description().get() : "");
                    emit error("Failed to get pilot ID", QString::fromStdString(result.error().message()), description);
                    return;
                }
            });
        });
    } else {
        _deleteFlightPlan();
    }
}

//-----------------------------------------------------------------------------
void
AirMapFlightPlanManager::_deleteFlightPlan()
{
    if(_flightsToDelete.count() < 1) {
        qCDebug(AirMapManagerLog) << "Delete non existing flight plan";
        return;
    }
    if(_state != State::Idle) {
        QTimer::singleShot(100, this, &AirMapFlightPlanManager::_deleteFlightPlan);
        return;
    }
    int idx = _flightList.findFlightPlanID(_flightsToDelete.last());
    if(idx >= 0) {
        AirspaceFlightInfo* pInfo = _flightList.get(idx);
        if(pInfo) {
            pInfo->setBeingDeleted(true);
        }
    }
    qCDebug(AirMapManagerLog) << "Deleting flight plan:" << _flightsToDelete.last();
    _state = State::FlightDelete;
    std::weak_ptr<LifetimeChecker> isAlive(_instance);
    FlightPlans::Delete::Parameters params;
    params.authorization = _shared.loginToken().toStdString();
    params.id = _flightsToDelete.last().toStdString();
    //-- Delete flight plan
    _shared.client()->flight_plans().delete_(params, [this, isAlive](const FlightPlans::Delete::Result& result) {
        if (!isAlive.lock()) return;
        if (_state != State::FlightDelete) return;
        if (result) {
            qCDebug(AirMapManagerLog) << "Flight plan deleted";
            _flightList.remove(_flightsToDelete.last());
        } else {
            QString description = QString::fromStdString(result.error().description() ? result.error().description().get() : "");
            emit error("Flight Plan deletion failed", QString::fromStdString(result.error().message()), description);
            AirspaceFlightInfo* pFlight = _flightList.get(_flightList.findFlightPlanID(_flightsToDelete.last()));
            if(pFlight) {
                pFlight->setBeingDeleted(false);
            }
        }
        _flightsToDelete.removeLast();
        //-- Keep at it until all flights are deleted
        //   TODO: This is ineficient. These whole airmapd transactions need to be moved into a separate
        //   worker thread.
        if(_flightsToDelete.count()) {
            QTimer::singleShot(10, this, &AirMapFlightPlanManager::_deleteFlightPlan);
        }
        _state = State::Idle;
    });
}

//-----------------------------------------------------------------------------
bool
AirMapFlightPlanManager::_collectFlightDtata()
{
    if(!_planController || !_planController->missionController()) {
        return false;
    }
    //-- Get flight bounding cube and prepare (box) polygon
    QGCGeoBoundingCube bc = *_planController->missionController()->travelBoundingCube();
    if(!bc.isValid() || !bc.area()) {
        //-- TODO: If single point, we need to set a point and a radius instead
        qCDebug(AirMapManagerLog) << "Not enough points for a flight plan.";
        return false;
    }
    _flight.maxAltitude   = fmax(bc.pointNW.altitude(), bc.pointSE.altitude());
    _flight.takeoffCoord  = _planController->missionController()->takeoffCoordinate();
    _flight.coords        = bc.polygon2D();
    _flight.bc            = bc;
    emit missionAreaChanged();
    //-- Flight Date/Time
    if(_flightStartTime.isNull() || _flightStartTime < QDateTime::currentDateTime()) {
        _flightStartTime = QDateTime::currentDateTime().addSecs(5 * 60);
        emit flightStartTimeChanged();
    }
    if(_flightEndTime.isNull() || _flightEndTime < _flightStartTime) {
        _flightEndTime = _flightStartTime.addSecs(30 * 60);
        emit flightEndTimeChanged();
    }
    return true;
}

//-----------------------------------------------------------------------------
void
AirMapFlightPlanManager::_createFlightPlan()
{
    _flight.reset();

    //-- Get flight data
    if(!_collectFlightDtata()) {
        return;
    }

    qCDebug(AirMapManagerLog) << "About to create flight plan";
    qCDebug(AirMapManagerLog) << "Takeoff:     " << _flight.takeoffCoord;
    qCDebug(AirMapManagerLog) << "Bounding box:" << _flight.bc.pointNW << _flight.bc.pointSE;
    qCDebug(AirMapManagerLog) << "Flight Start:" << _flightStartTime;
    qCDebug(AirMapManagerLog) << "Flight End:  " << _flightEndTime;

    //-- Not Yet
    //return;

    if (_pilotID == "") {
        //-- Need to get the pilot id before uploading the flight plan
        qCDebug(AirMapManagerLog) << "Getting pilot ID";
        _state = State::GetPilotID;
        std::weak_ptr<LifetimeChecker> isAlive(_instance);
        _shared.doRequestWithLogin([this, isAlive](const QString& login_token) {
            if (!isAlive.lock()) return;
            Pilots::Authenticated::Parameters params;
            params.authorization = login_token.toStdString();
            _shared.client()->pilots().authenticated(params, [this, isAlive](const Pilots::Authenticated::Result& result) {
                if (!isAlive.lock()) return;
                if (_state != State::GetPilotID) return;
                if (result) {
                    _pilotID = QString::fromStdString(result.value().id);
                    qCDebug(AirMapManagerLog) << "Got Pilot ID:"<<_pilotID;
                    _state = State::Idle;
                    _uploadFlightPlan();
                } else {
                    _flightPermitStatus = AirspaceFlightPlanProvider::PermitNone;
                    emit flightPermitStatusChanged();
                    _state = State::Idle;
                    QString description = QString::fromStdString(result.error().description() ? result.error().description().get() : "");
                    emit error("Failed to create Flight Plan", QString::fromStdString(result.error().message()), description);
                    return;
                }
            });
        });
    } else {
        _uploadFlightPlan();
    }

    _flightPermitStatus = AirspaceFlightPlanProvider::PermitPending;
    emit flightPermitStatusChanged();
}

//-----------------------------------------------------------------------------
void
AirMapFlightPlanManager::_uploadFlightPlan()
{
    qCDebug(AirMapManagerLog) << "Uploading flight plan";
    if(_state != State::Idle) {
        QTimer::singleShot(100, this, &AirMapFlightPlanManager::_uploadFlightPlan);
        return;
    }
    _state = State::FlightUpload;
    std::weak_ptr<LifetimeChecker> isAlive(_instance);
    _shared.doRequestWithLogin([this, isAlive](const QString& login_token) {
        if (!isAlive.lock()) return;
        if (_state != State::FlightUpload) return;
        FlightPlans::Create::Parameters params;
        params.max_altitude = _flight.maxAltitude;
        params.min_altitude = 0.0;
        params.buffer       = 2.f;
        params.latitude     = _flight.takeoffCoord.latitude();
        params.longitude    = _flight.takeoffCoord.longitude();
        params.pilot.id     = _pilotID.toStdString();
        quint64 start       = _flightStartTime.toUTC().toMSecsSinceEpoch();
        quint64 end         = _flightEndTime.toUTC().toMSecsSinceEpoch();
        params.start_time   = airmap::from_milliseconds_since_epoch(airmap::Milliseconds{(long long)start});
        params.end_time     = airmap::from_milliseconds_since_epoch(airmap::Milliseconds{(long long)end});
        //-- Rules
        AirMapRulesetsManager* pRulesMgr = dynamic_cast<AirMapRulesetsManager*>(qgcApp()->toolbox()->airspaceManager()->ruleSets());
        if(pRulesMgr) {
            for(int rs = 0; rs < pRulesMgr->ruleSets()->count(); rs++) {
                AirMapRuleSet* ruleSet = qobject_cast<AirMapRuleSet*>(pRulesMgr->ruleSets()->get(rs));
                //-- If this ruleset is selected
                if(ruleSet && ruleSet->selected()) {
                    params.rulesets.push_back(ruleSet->id().toStdString());
                    //-- Features within each rule
                    /*
                    for(int r = 0; r < ruleSet->rules()->count(); r++) {
                        AirMapRule* rule = qobject_cast<AirMapRule*>(ruleSet->rules()->get(r));
                        if(rule) {
                            for(int f = 0; f < rule->features()->count(); f++) {
                                AirMapRuleFeature* feature = qobject_cast<AirMapRuleFeature*>(rule->features()->get(f));
                                if(feature && feature->value().isValid()) {
                                    switch(feature->type()) {
                                    case AirspaceRuleFeature::Boolean:
                                        params.features[feature->name().toStdString()] = RuleSet::Feature::Value(feature->value().toBool());
                                        break;
                                    case AirspaceRuleFeature::Float:
                                        params.features[feature->name().toStdString()] = RuleSet::Feature::Value(feature->value().toFloat());
                                        break;
                                    case AirspaceRuleFeature::String:
                                        params.features[feature->name().toStdString()] = RuleSet::Feature::Value(feature->value().toString().toStdString());
                                        break;
                                    default:
                                        qCWarning(AirMapManagerLog) << "Unknown type for feature" << feature->name();
                                    }
                                }
                            }
                        }
                    }
                    */
                }
            }
        }
        //-- Geometry: LineString
        Geometry::LineString lineString;
        for (const auto& qcoord : _flight.coords) {
            Geometry::Coordinate coord;
            coord.latitude  = qcoord.latitude();
            coord.longitude = qcoord.longitude();
            lineString.coordinates.push_back(coord);
        }
        params.geometry = Geometry(lineString);
        params.authorization = login_token.toStdString();
        //-- Create flight plan
        _shared.client()->flight_plans().create_by_polygon(params, [this, isAlive](const FlightPlans::Create::Result& result) {
            if (!isAlive.lock()) return;
            if (_state != State::FlightUpload) return;
            if (result) {
                _flightPlan = QString::fromStdString(result.value().id);
                qCDebug(AirMapManagerLog) << "Flight plan created:" << _flightPlan;
                _state = State::FlightPlanPolling;
                _pollBriefing();
            } else {
                _state = State::Idle;
                QString description = QString::fromStdString(result.error().description() ? result.error().description().get() : "");
                emit error("Flight Plan creation failed", QString::fromStdString(result.error().message()), description);
            }
        });
    });
}

//-----------------------------------------------------------------------------
void
AirMapFlightPlanManager::_updateFlightPlan()
{
    //-- TODO: This is broken as the parameters for updating the plan have
    //   little to do with those used when creating it.

    qCDebug(AirMapManagerLog) << "Updating flight plan";

    if(_state != State::Idle) {
        QTimer::singleShot(100, this, &AirMapFlightPlanManager::_updateFlightPlan);
        return;
    }
    //-- Get flight data
    if(!_collectFlightDtata()) {
        return;
    }

    qCDebug(AirMapManagerLog) << "Takeoff:     " << _flight.takeoffCoord;
    qCDebug(AirMapManagerLog) << "Bounding box:" << _flight.bc.pointNW << _flight.bc.pointSE;
    qCDebug(AirMapManagerLog) << "Flight Start:" << _flightStartTime;
    qCDebug(AirMapManagerLog) << "Flight End:  " << _flightEndTime;

    _state = State::FlightUpdate;
    std::weak_ptr<LifetimeChecker> isAlive(_instance);
    _shared.doRequestWithLogin([this, isAlive](const QString& login_token) {
        if (!isAlive.lock()) return;
        if (_state != State::FlightUpdate) return;
        FlightPlans::Update::Parameters params = {};
        params.authorization                 = login_token.toStdString();
        params.flight_plan.id                = _flightPlan.toStdString();
        params.flight_plan.pilot.id          = _pilotID.toStdString();
        params.flight_plan.altitude_agl.max  = _flight.maxAltitude;
        params.flight_plan.altitude_agl.min  = 0.0f;
        params.flight_plan.buffer            = 2.f;
        params.flight_plan.takeoff.latitude  = _flight.takeoffCoord.latitude();
        params.flight_plan.takeoff.longitude = _flight.takeoffCoord.longitude();
        quint64 start                        = _flightStartTime.toUTC().toMSecsSinceEpoch();
        quint64 end                          = _flightEndTime.toUTC().toMSecsSinceEpoch();
        params.flight_plan.start_time        = airmap::from_milliseconds_since_epoch(airmap::Milliseconds{(long long)start});
        params.flight_plan.end_time          = airmap::from_milliseconds_since_epoch(airmap::Milliseconds{(long long)end});
        //-- Rules
        /*
        AirMapRulesetsManager* pRulesMgr = dynamic_cast<AirMapRulesetsManager*>(qgcApp()->toolbox()->airspaceManager()->ruleSets());
        if(pRulesMgr) {
            foreach(QString ruleset, pRulesMgr->rulesetsIDs()) {
                params.flight_plan.rulesets.push_back(ruleset.toStdString());
            }
        }
        */
        //-- Geometry: LineString
        Geometry::LineString lineString;
        for (const auto& qcoord : _flight.coords) {
            Geometry::Coordinate coord;
            coord.latitude  = qcoord.latitude();
            coord.longitude = qcoord.longitude();
            lineString.coordinates.push_back(coord);
        }
        params.flight_plan.geometry = Geometry(lineString);
        //-- Update flight plan
        _shared.client()->flight_plans().update(params, [this, isAlive](const FlightPlans::Update::Result& result) {
            if (!isAlive.lock()) return;
            if (_state != State::FlightUpdate) return;
            if (result) {
                _state = State::FlightPlanPolling;
                qCDebug(AirMapManagerLog) << "Flight plan updated:" << _flightPlan;
                _pollBriefing();
            } else {
                _state = State::Idle;
                QString description = QString::fromStdString(result.error().description() ? result.error().description().get() : "");
                emit error("Flight Plan update failed", QString::fromStdString(result.error().message()), description);
            }
        });
    });
}

//-----------------------------------------------------------------------------
static bool
adv_sort(QObject* a, QObject* b)
{
    AirMapAdvisory* aa = qobject_cast<AirMapAdvisory*>(a);
    AirMapAdvisory* bb = qobject_cast<AirMapAdvisory*>(b);
    if(!aa || !bb) return false;
    return (int)aa->color() > (int)bb->color();
}

//-----------------------------------------------------------------------------
static bool
rules_sort(QObject* a, QObject* b)
{
    AirMapRule* aa = qobject_cast<AirMapRule*>(a);
    AirMapRule* bb = qobject_cast<AirMapRule*>(b);
    if(!aa || !bb) return false;
    return (int)aa->status() > (int)bb->status();
}

//-----------------------------------------------------------------------------
void
AirMapFlightPlanManager::_pollBriefing()
{
    if (_state != State::FlightPlanPolling && _state != State::FlightPolling) {
        return;
    }
    FlightPlans::RenderBriefing::Parameters params;
    params.authorization = _shared.loginToken().toStdString();
    params.id            = _flightPlan.toStdString();
    std::weak_ptr<LifetimeChecker> isAlive(_instance);
    _shared.client()->flight_plans().render_briefing(params, [this, isAlive](const FlightPlans::RenderBriefing::Result& result) {
        if (!isAlive.lock()) return;
        if (_state != State::FlightPlanPolling && _state != State::FlightPolling) return;
        if (result) {
            const FlightPlan::Briefing& briefing = result.value();
            qCDebug(AirMapManagerLog) << "Flight polling/briefing response";
            //-- Collect advisories
            _valid = false;
            _advisories.clearAndDeleteContents();
            const std::vector<Status::Advisory> advisories = briefing.airspace.advisories;
            _airspaceColor = (AirspaceAdvisoryProvider::AdvisoryColor)(int)briefing.airspace.color;
            for (const auto& advisory : advisories) {
                AirMapAdvisory* pAdvisory = new AirMapAdvisory(this);
                pAdvisory->_id          = QString::fromStdString(advisory.airspace.id());
                pAdvisory->_name        = QString::fromStdString(advisory.airspace.name());
                pAdvisory->_type        = (AirspaceAdvisory::AdvisoryType)(int)advisory.airspace.type();
                pAdvisory->_color       = (AirspaceAdvisoryProvider::AdvisoryColor)(int)advisory.color;
                _advisories.append(pAdvisory);
                qCDebug(AirMapManagerLog) << "Adding briefing advisory" << pAdvisory->name();
            }
            //-- Sort in order of color (priority)
            _advisories.beginReset();
            std::sort(_advisories.objectList()->begin(), _advisories.objectList()->end(), adv_sort);
            _advisories.endReset();
            _valid = true;
            //-- Collect Rulesets
            _authorizations.clearAndDeleteContents();
            _rulesViolation.clearAndDeleteContents();
            _rulesInfo.clearAndDeleteContents();
            _rulesReview.clearAndDeleteContents();
            _rulesFollowing.clearAndDeleteContents();
            _briefFeatures.clear();
            for(const auto& ruleset : briefing.evaluation.rulesets) {
                AirMapRuleSet* pRuleSet = new AirMapRuleSet(this);
                pRuleSet->_id = QString::fromStdString(ruleset.id);
                //-- Iterate Rules
                for (const auto& rule : ruleset.rules) {
                    AirMapRule* pRule = new AirMapRule(rule, this);
                    //-- Iterate Rule Features
                    for (const auto& feature : rule.features) {
                        AirMapRuleFeature* pFeature = new AirMapRuleFeature(feature, this);
                        pRule->_features.append(pFeature);
                        if(rule.status == RuleSet::Rule::Status::missing_info) {
                            _briefFeatures.append(pFeature);
                            qCDebug(AirMapManagerLog) << "Adding briefing feature" << pFeature->name() << pFeature->description() << pFeature->type();
                        }
                    }
                    pRuleSet->_rules.append(pRule);
                    //-- Rules separated by status for presentation
                    switch(rule.status) {
                    case RuleSet::Rule::Status::conflicting:
                        _rulesViolation.append(new AirMapRule(rule, this));
                        break;
                    case RuleSet::Rule::Status::not_conflicting:
                        _rulesFollowing.append(new AirMapRule(rule, this));
                        break;
                    case RuleSet::Rule::Status::missing_info:
                        _rulesInfo.append(new AirMapRule(rule, this));
                        break;
                    case RuleSet::Rule::Status::informational:
                        _rulesReview.append(new AirMapRule(rule, this));
                        break;
                    default:
                        break;
                    }
                }
                //-- Sort rules by relevance order
                pRuleSet->_rules.beginReset();
                std::sort(pRuleSet->_rules.objectList()->begin(), pRuleSet->_rules.objectList()->end(), rules_sort);
                pRuleSet->_rules.endReset();
                _rulesets.append(pRuleSet);
                qCDebug(AirMapManagerLog) << "Adding briefing ruleset" << pRuleSet->id();
            }
            //-- Evaluate briefing status
            bool rejected = false;
            bool accepted = false;
            bool pending  = false;
            for (const auto& authorization : briefing.evaluation.authorizations) {
                AirMapFlightAuthorization* pAuth = new AirMapFlightAuthorization(authorization, this);
                _authorizations.append(pAuth);
                qCDebug(AirMapManagerLog) << "Autorization:" << pAuth->name() << " (" << pAuth->message() << ")" << (int)pAuth->status();
                switch (authorization.status) {
                case Evaluation::Authorization::Status::accepted:
                case Evaluation::Authorization::Status::accepted_upon_submission:
                    accepted = true;
                    break;
                case Evaluation::Authorization::Status::rejected:
                case Evaluation::Authorization::Status::rejected_upon_submission:
                    rejected = true;
                    break;
                case Evaluation::Authorization::Status::pending:
                    pending = true;
                    break;
                //-- If we don't know, accept it
                default:
                    accepted = true;
                    break;
                }
            }
            if (briefing.evaluation.authorizations.size() == 0) {
                // If we don't get any authorizations, we assume it's accepted
                accepted = true;
            }
            emit advisoryChanged();
            emit rulesChanged();
            qCDebug(AirMapManagerLog) << "Flight approval: accepted=" << accepted << "rejected" << rejected << "pending" << pending;
            if ((rejected || accepted) && !pending) {
                if (rejected) { // rejected has priority
                    _flightPermitStatus = AirspaceFlightPlanProvider::PermitRejected;
                } else {
                    _flightPermitStatus = AirspaceFlightPlanProvider::PermitAccepted;
                }
                emit flightPermitStatusChanged();
                _state = State::Idle;
            } else {
                //-- Pending. Try again.
                _pollTimer.setSingleShot(true);
                _pollTimer.start(1000);
            }
        } else {
            _state = State::Idle;
            QString description = QString::fromStdString(result.error().description() ? result.error().description().get() : "");
            emit error("Brief Request failed",
                    QString::fromStdString(result.error().message()), description);
        }
    });
}

//-----------------------------------------------------------------------------
void
AirMapFlightPlanManager::_missionChanged()
{
    //-- Are we enabled?
    if(!qgcApp()->toolbox()->settingsManager()->airMapSettings()->enableAirMap()->rawValue().toBool()) {
        return;
    }
    //-- Do we have a license?
    if(!_shared.hasAPIKey()) {
        return;
    }
    //-- Creating a new flight plan?
    if(_state == State::Idle) {
        if(_flightPlan.isEmpty()) {
            _createFlightPlan();
        } else {
            //-- Plan is being modified
            _updateFlightPlan();
        }
    }
}

//-----------------------------------------------------------------------------
void
AirMapFlightPlanManager::loadFlightList(QDateTime startTime, QDateTime endTime)
{
    //-- TODO: This is not checking if the state is Idle. Again, these need to
    //   queued up and handled by a worker thread.
    qCDebug(AirMapManagerLog) << "Preparing load flight list";
    _loadingFlightList = true;
    emit loadingFlightListChanged();
    _rangeStart = startTime;
    _rangeEnd   = endTime;
    qCDebug(AirMapManagerLog) << "List flights from:" << _rangeStart.toString("yyyy MM dd - hh:mm:ss") << "to" << _rangeEnd.toString("yyyy MM dd - hh:mm:ss");
    if (_pilotID == "") {
        //-- Need to get the pilot id
        qCDebug(AirMapManagerLog) << "Getting pilot ID";
        _state = State::GetPilotID;
        std::weak_ptr<LifetimeChecker> isAlive(_instance);
        _shared.doRequestWithLogin([this, isAlive](const QString& login_token) {
            if (!isAlive.lock()) return;
            Pilots::Authenticated::Parameters params;
            params.authorization = login_token.toStdString();
            _shared.client()->pilots().authenticated(params, [this, isAlive](const Pilots::Authenticated::Result& result) {
                if (!isAlive.lock()) return;
                if (_state != State::GetPilotID) return;
                if (result) {
                    _pilotID = QString::fromStdString(result.value().id);
                    qCDebug(AirMapManagerLog) << "Got Pilot ID:"<<_pilotID;
                    _state = State::Idle;
                    _loadFlightList();
                } else {
                    _state = State::Idle;
                    QString description = QString::fromStdString(result.error().description() ? result.error().description().get() : "");
                    emit error("Failed to get pilot ID", QString::fromStdString(result.error().message()), description);
                    _loadingFlightList = false;
                    emit loadingFlightListChanged();
                    return;
                }
            });
        });
    } else {
        _loadFlightList();
    }
}

//-----------------------------------------------------------------------------
void
AirMapFlightPlanManager::_loadFlightList()
{
    qCDebug(AirMapManagerLog) << "Load flight list";
    if(_state != State::Idle) {
        QTimer::singleShot(100, this, &AirMapFlightPlanManager::_loadFlightList);
        return;
    }
    _flightList.clear();
    emit flightListChanged();
    _state = State::LoadFlightList;
    std::weak_ptr<LifetimeChecker> isAlive(_instance);
    _shared.doRequestWithLogin([this, isAlive](const QString& login_token) {
        if (!isAlive.lock()) return;
        if (_state != State::LoadFlightList) return;
        Flights::Search::Parameters params;
        params.authorization = login_token.toStdString();
        quint64 start   = _rangeStart.toUTC().toMSecsSinceEpoch();
        quint64 end     = _rangeEnd.toUTC().toMSecsSinceEpoch();
        params.start_after  = airmap::from_milliseconds_since_epoch(airmap::Milliseconds{(long long)start});
        params.start_before = airmap::from_milliseconds_since_epoch(airmap::Milliseconds{(long long)end});
        params.limit    = 250;
        params.pilot_id = _pilotID.toStdString();
        qCDebug(AirMapManagerLog) << "List flights from:" << _rangeStart.toUTC().toString("yyyy MM dd - hh:mm:ss") << "to" << _rangeEnd.toUTC().toString("yyyy MM dd - hh:mm:ss");
        _shared.client()->flights().search(params, [this, isAlive](const Flights::Search::Result& result) {
            if (!isAlive.lock()) return;
            if (_state != State::LoadFlightList) return;
            if (result && result.value().flights.size() > 0) {
                const Flights::Search::Response& response = result.value();
                for (const auto& flight : response.flights) {
                    AirMapFlightInfo* pFlight = new AirMapFlightInfo(flight, this);
                    _flightList.append(pFlight);
                    qCDebug(AirMapManagerLog) << "Found:" << pFlight->flightID() << pFlight->flightPlanID();
                }
                emit flightListChanged();
            } else {
                if(!result) {
                    QString description = QString::fromStdString(result.error().description() ? result.error().description().get() : "");
                    emit error("Flight search failed", QString::fromStdString(result.error().message()), description);
                }
            }
            _state = State::Idle;
            _loadingFlightList = false;
            emit loadingFlightListChanged();
        });
    });
}
