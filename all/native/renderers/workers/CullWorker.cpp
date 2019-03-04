#include "CullWorker.h"
#include "layers/Layer.h"
#include "projections/ProjectionSurface.h"
#include "renderers/MapRenderer.h"
#include "utils/Const.h"
#include "utils/Log.h"
#include "utils/ThreadUtils.h"

#include <algorithm>

namespace carto {

    CullWorker::CullWorker() :
        _layerWakeupMap(),
        _firstCull(true),
        _envelope(),
        _viewState(),
        _mapRenderer(),
        _stop(false),
        _idle(false),
        _condition(),
        _mutex()
    {
    }
    
    CullWorker::~CullWorker() {
    }
        
    void CullWorker::setComponents(const std::weak_ptr<MapRenderer>& mapRenderer, const std::shared_ptr<CullWorker>& worker) {
        _mapRenderer = mapRenderer;
        // When the map component gets destroyed all threads get detatched. Detatched threads need their worker objects to be alive,
        // so worker objects need to keep references to themselves, until the loop finishes.
        _worker = worker;
    }
    
    void CullWorker::init(const std::shared_ptr<Layer>& layer, int delayTime) {
        std::lock_guard<std::mutex> lock(_mutex);
    
        std::chrono::steady_clock::time_point wakeupTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayTime);
        if (_layerWakeupMap.find(layer) != _layerWakeupMap.end()) {
            if (_layerWakeupMap[layer] <= wakeupTime) {
                return;
            }
        }
        _layerWakeupMap[layer] = wakeupTime;
        _idle = false;
        _condition.notify_one();
    }
    
    void CullWorker::stop() {
        std::lock_guard<std::mutex> lock(_mutex);
        _stop = true;
        _condition.notify_all();
    }
    
    bool CullWorker::isIdle() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _idle;
    }
    
    void CullWorker::operator ()() {
        run();
        _worker.reset();
    }
        
    void CullWorker::run() {
        ThreadUtils::SetThreadPriority(ThreadPriority::LOW);
        while (true) {
            std::vector<std::shared_ptr<Layer> > layers;
            {
                std::unique_lock<std::mutex> lock(_mutex);
                
                if (_stop) {
                    return;
                }

                std::chrono::steady_clock::time_point currentTime = std::chrono::steady_clock::now();
                std::chrono::steady_clock::time_point wakeupTime = std::chrono::steady_clock::now() + std::chrono::hours(24);
                for (auto it = _layerWakeupMap.begin(); it != _layerWakeupMap.end(); ) {
                    if (it->second - currentTime < std::chrono::milliseconds(1)) {
                        layers.push_back(it->first);
                        it = _layerWakeupMap.erase(it);
                    } else {
                        wakeupTime = std::min(wakeupTime, it->second);
                        it++;
                    }
                }
                
                if (layers.empty()) {
                    _idle = _layerWakeupMap.empty();
                    _condition.wait_for(lock, wakeupTime - std::chrono::steady_clock::now());
                    _idle = false;
                }
            }

            if (!layers.empty()) {
                const std::shared_ptr<MapRenderer>& mapRenderer = _mapRenderer.lock();
                if (!mapRenderer) {
                    return;
                }
                
                // Get view state
                const ViewState& viewState = mapRenderer->getViewState();
                if (viewState.getWidth() <= 0 || viewState.getHeight() <= 0) {
                    continue;
                }
                
                // Check if view state has changed
                if (_firstCull || viewState.getModelviewProjectionMat() != _viewState.getModelviewProjectionMat() || viewState.getProjectionSurface() != _viewState.getProjectionSurface()) {
                    _firstCull = false;
                    _viewState = viewState;
                    
                    // Calculate state
                    calculateCullState();
                }
                
                // Update layers
                updateLayers(layers);
            }
        }
    }
    
    void CullWorker::calculateCullState() {
        // Calculate envelope for the visible frustum
        calculateEnvelope();
    }
    
    void CullWorker::calculateEnvelope() {
        std::shared_ptr<ProjectionSurface> projectionSurface = _viewState.getProjectionSurface();
        if (!projectionSurface) {
            return;
        }

        cglib::mat4x4<double> invMVPMat = cglib::inverse(_viewState.getModelviewProjectionMat());

        // Find closest points to screen corners
        cglib::vec2<float> screenPoses[] = {
            cglib::vec2<float>(-VIEWPORT_SCALE, -VIEWPORT_SCALE),
            cglib::vec2<float>(-VIEWPORT_SCALE,  VIEWPORT_SCALE),
            cglib::vec2<float>( VIEWPORT_SCALE,  VIEWPORT_SCALE),
            cglib::vec2<float>( VIEWPORT_SCALE, -VIEWPORT_SCALE)
        };

        MapPos mapPoses[4];
        for (int i = 0; i < 4; i++) {
            cglib::vec3<double> pos0 = cglib::transform_point(cglib::vec3<double>(screenPoses[i](0), screenPoses[i](1), -1), invMVPMat);
            cglib::vec3<double> pos1 = cglib::transform_point(cglib::vec3<double>(screenPoses[i](0), screenPoses[i](1),  1), invMVPMat);
            cglib::ray3<double> ray(pos0, pos1 - pos0);

            double t = 0;
            mapPoses[i] = projectionSurface->calculateMapPos(projectionSurface->calculateNearestPoint(ray, 0, t));
        }

        // Build envelope, tesselate envelope edges
        std::vector<MapPos> envelopePoses;
        envelopePoses.reserve(8);
        for (int i = 0; i < 4; i++) {
            if (!envelopePoses.empty()) {
                envelopePoses.pop_back();
            }
            projectionSurface->tesselateSegment(mapPoses[i], mapPoses[(i + 1) % 4], envelopePoses);
        }

        // Check for X coordinate wrapping. Do quick fix of the poles.
        bool addPole1 = _viewState.getFrustum().inside(projectionSurface->calculatePosition(MapPos(0, -std::numeric_limits<double>::infinity())));
        bool addPole2 = _viewState.getFrustum().inside(projectionSurface->calculatePosition(MapPos(0,  std::numeric_limits<double>::infinity())));
        if (addPole1 || addPole2) {
            double minX = -Const::WORLD_SIZE * 0.5;
            double maxX =  Const::WORLD_SIZE * 0.5;
            double minY = std::min_element(envelopePoses.begin(), envelopePoses.end(), [](const MapPos& p1, const MapPos& p2) { return p1.getY() < p2.getY(); })->getY();
            double maxY = std::max_element(envelopePoses.begin(), envelopePoses.end(), [](const MapPos& p1, const MapPos& p2) { return p1.getY() < p2.getY(); })->getY();
            if (addPole1) {
                minY = -std::numeric_limits<double>::infinity();
            }
            if (addPole2) {
                maxY =  std::numeric_limits<double>::infinity();
            }

            envelopePoses.clear();
            envelopePoses.emplace_back(minX, minY);
            envelopePoses.emplace_back(minX, maxY);
            envelopePoses.emplace_back(maxX, maxY);
            envelopePoses.emplace_back(maxX, minY);
        }

        // Check if poles are visible. We
        _envelope = MapEnvelope(envelopePoses);
    }
    
    void CullWorker::updateLayers(const std::vector<std::shared_ptr<Layer> >& layers) {
        for (const std::shared_ptr<Layer>& layer : layers) {
            layer->update(std::make_shared<CullState>(_envelope, _viewState));
        }
    }
    
    const float CullWorker::VIEWPORT_SCALE = 1.1f; // enlarge viewport envelope by approx. 10%
}
