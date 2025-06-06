/**
 *    Copyright (C) 2021-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */
#pragma once

#include "mongo/db/process_health/health_observer.h"
#include "mongo/db/service_context.h"

#include <functional>
#include <memory>
#include <vector>

namespace mongo {
namespace process_health {

/**
 * Registration mechanism for all health observers.
 * This is static class not requiring an instance to work.
 */
class HealthObserverRegistration {
public:
    /**
     * Registers a factory method, which will be invoked later to instantiate the observer.
     * This must be invoked by static initializers, the code is not internally synchronized.
     *
     * @param factoryCallback creates observer instance when invoked.
     */
    static void registerObserverFactory(
        std::function<std::unique_ptr<HealthObserver>(ServiceContext* svcCtx)> factoryCallback);

    /**
     * Invokes all registered factories and returns new instances.
     * The ownership of all observers is transferred to the invoker.
     */
    static std::vector<std::unique_ptr<HealthObserver>> instantiateAllObservers(
        ServiceContext* svcCtx);

    /**
     * Test-only method to cleanup the list of registered factories.
     */
    static void resetObserverFactoriesForTest();
};

}  // namespace process_health
}  // namespace mongo
