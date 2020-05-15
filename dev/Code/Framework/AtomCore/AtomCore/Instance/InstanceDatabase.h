/*
* All or portions of this file Copyright (c) Amazon.com, Inc. or its affiliates or
* its licensors.
*
* For complete copyright and license terms please see the LICENSE at the root of this
* distribution (the "License"). All use of this software is governed by the License,
* or, if provided, by the license below or the license accompanying this file. Do not
* remove or modify any license notices. This file is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*
*/

#pragma once

#include <AtomCore/Instance/Instance.h>
#include <AtomCore/Instance/InstanceData.h>
#include <AtomCore/Instance/InstanceId.h>

#include <AzCore/Asset/AssetManager.h>
#include <AzCore/Asset/AssetCommon.h>
#include <AzCore/Module/Environment.h>
#include <AzCore/std/parallel/shared_mutex.h>

namespace AZ
{
    namespace Data
    {
        /**
         * Provides create and delete functions for a specific InstanceData type, for use by @ref InstanceDatabase
         */
        template <typename Type>
        struct InstanceHandler
        {
            /**
             * Creation takes an asset as input and produces a new instance as output.
             * Ownership must be returned to the caller. Use this method to perform
             * both allocation and initialization using the provided asset. The returned
             * instance is assumed to be valid and usable by the client.
             *
             * Usage Examples:
             *  - The user may choose to allocate from a local pool or cache.
             *  - The concrete instance type may have a non-standard initialization path.
             *  - The user may wish to encode global context into the functor (an RHI device, for example).
             *
             *  PERFORMANCE NOTE: Creation is currently done under a lock. Initialization should be quick.
             */
            using CreateFunction = AZStd::function<Instance<Type>(AssetData*)>;

            /**
             * Deletion takes an asset as input and transfers ownership to the method.
             */
            using DeleteFunction = AZStd::function<void(Type*)>;

            /// [Required] The function to use when creating an instance.
            ///  The system will assert if no creation function is provided.
            CreateFunction m_createFunction;

            /// [Optional] The function to use when deleting an instance.
            DeleteFunction m_deleteFunction = [](Type* t) { delete t; };
        };

        //! This class exists to allow InstanceData to access parts of InstanceDatabase without having
        //! to know the instance data type, since InstanceDatabase is a template class.
        class InstanceDatabaseInterface
        {
            friend class InstanceData;
        protected:
            virtual void ReleaseInstance(InstanceData* instance, const InstanceId& instanceId) = 0;
        };

        /**
         * This class is a simple database of typed instances. An 'instance' in this context is any class
         * which inherits from InstanceData, is created at runtime from an asset, and has a unique instance
         * id. The purpose of this system is to control de-duplication of instances at runtime, and to 
         * associate instance types with their originating asset types.
         *
         * The database has itself singleton access, but it should be owned by the corresponding system (which
         * is in charge of creation / destruction of the database). To use the database, you may instantiate it
         * using one of the following approaches:
         *  1) Instantiate one InstanceDatabase for each concrente instance class. Use this approach if all
         *     concrete instance classes are known at compile time.
         *  2) Instantiate one InstanceDatabase for a known instance base class, and then register multiple
         *     InstanceHandlers for each concrete instance class. Use this approach if only the instance base
         *     class is known at compile time and the concrete instance classes are only known at runtime.
         *     For example, Atom provides abstract StreamingImageControllerAsset and StreamingImageController
         *     classes, and a game-specific gem can provide custom implementations by adding a handler to
         *     InstanceDatabase<StreamingImageController>.
         *
         * The database allows you to find an instance from its corresponding @ref InstanceId. Alternatively, you
         * can 'find or create' an instance, which will create the instance if it doesn't already exist, or return you the
         * existing instance. The 'find or create' operation takes an asset as input. Instances are designed to be trivially
         * created from their parent asset.
         *
         * The database does NOT own instances. Ownership is returned to you in the form of a smart pointer (Data::Instance<>).
         * This is the same ownership model used by the asset manager.
         *
         * The system is thread-safe. You can create / destroy instances from any thread, however Instances should not be
         * copied between threads, they should always be retrieved from the InstanceDatabase directly.
         *
         * Example Usage (using instantiation approach #1 described above):
         * @code{.cpp}
         *
         * // Create the database.
         * Data::InstanceHandler<MyInstanceType> handler;
         *
         * // Provide your own creator (and optional deleter) to control allocation / initialization of your object.
         * handler.m_createFunction = [] (Data::AssetData* assetData) { return aznew MyInstanceType(assetData); };
         *
         * Data::InstanceDatabase<MyInstanceType>::Create(azrtti_typeid<MyAssetType>(), handler);
         *
         * Data::Asset<MyAssetType> myAsset{ASSETID_1};
         *
         * // Create an instance id from the asset id (1-to-1 mapping).
         * Data::InstanceId instanceId = Data::InstanceId::CreateFromAssetId(myAsset.GetId());
         *
         * // Find or create an instance from an asset.
         * Data::Instance<MyInstanceType> instance = Data::InstanceDatabase<MyInstanceType>::Instance().FindOrCreate(instanceId, myAsset);
         *
         * // Create an instance by name.
         * Data::InstanceId instanceIdName = Data::InstanceId::CreateName("HelloWorld");
         *
         * // Creates a new instance from the same asset (the old instance is de-ref'd).
         * instance = Data::InstanceDatabase<MyInstanceType>::Instance().FindOrCreate(instanceIdName, myAsset);
         *
         * // Finds an existing instance.
         * Data::Instance<MyInstanceType> instance2 = Data::InstanceDatabase<MyInstanceType>::Instance().Find(instanceIdName);
         *
         * instance == instance2; // true
         *
         * // Find or create an existing instance.
         * Data::Instance<MyInstanceType> instance3 = Data::InstanceDatabase<MyInstanceType>::Instance().FindOrCreate(instanceIdName, myAsset);
         *
         * instance == instance2 == instance3; // true
         *
         * // INVALID: Create an instance using a different asset.
         * Data::Asset<MyAssetType> myAsset2{ASSETID_2};
         *
         * // This will assert. You can only request an instance using the SAME asset each time. If the system detects a mismatch it
         * // will throw an error.
         * Data::Instance<MyInstanceType> instance3 = Data::InstanceDatabase<MyInstanceType>::Instance().FindOrCreate(instanceIdName, myAsset2);
         *
         * // After all objects are out of scope! The system will report an error if objects are still active on destruction.
         * Data::InstanceDatabase<MyInstanceType>::Destroy();
         *
         * @endcode
         */
        template <typename Type>
        class InstanceDatabase final : public InstanceDatabaseInterface
        {
            static_assert(AZStd::is_base_of<InstanceData, Type>::value, "Type must inherit from Data::Instance to be used in Data::InstanceDatabase.");
        public:
            AZ_CLASS_ALLOCATOR(InstanceDatabase, AZ::SystemAllocator, 0);

            /**
             * Create the InstanceDatabase with a single handler.
             * Use this function when creating an InstanceDatabase that will handle concrete classes of @ref Type.
             * \param assetType  - All instances will be based on subclasses of this asset type.
             * \param handler    - An InstanceHandler that creates instances of @ref assetType assets.
             */
            static void Create(const AssetType& assetType, const InstanceHandler<Type>& handler);

            /**
             * Create the InstanceDatabase with no handlers. Individual handlers must be added using @ref AddHandler().
             * Use this function when creating an InstanceDatabase that will handle subclasses of @ref Type.
             * \param assetType - All instances will be based on subclasses of this asset type.
             */
            static void Create(const AssetType& assetType);

            static void Destroy();
            static bool IsReady();
            static InstanceDatabase& Instance();

            /**
             * Add an InstanceHandler that will create instances for assets of type @ref assetType.
             */
            void AddHandler(const AssetType& assetType, const InstanceHandler<Type>& handler);
            void AddHandler(const AssetType& assetType, typename InstanceHandler<Type>::CreateFunction createFunction);

            void RemoveHandler(const AssetType& assetType);

            /**
             * Attempts to find an instance associated with the provided id. If the instance exists, it
             * is returned. If no instance is found, nullptr is returned. If is safe to call this from
             * multiple threads.
             *
             * @param id The id used to find an instance in the database.
             */
            Data::Instance<Type> Find(const InstanceId& id) const;

            /**
             * Attempts to find an instance associated with the provided id. If it exists, it is returned.
             * Otherwise, it is created using the provided asset data and then returned. It is safe to call
             * this method from multiple threads, even with the same id. The call is synchronous and other threads
             * will block until creation is complete.
             *
             * PERFORMANCE NOTE: If the asset data is not loaded and creation is required, the system will
             * perform a BLOCKING load on the asset. If this behavior is not desired, the user should either
             * ensure the asset is loaded prior to calling this method, or call @ref Find instead.
             *
             * @param id The id used to find or create an instance in the database.
             * @param asset The asset used to initialize the instance, if it does NOT already exist.
             *      If the instance exists, the asset id is checked against the existing instance. If
             *      validation is enabled, the system will error if the created asset id does not match
             *      the provided asset id. It is required that you consistently provide the same asset
             *      when acquiring an instance.
             * @return Returns a smart pointer to the instance, which was either found or created.
             */
            Data::Instance<Type> FindOrCreate(const InstanceId& id, const Asset<AssetData>& asset);

            //! Calls the above FindOrCreate using an InstanceId created from the asset
            Data::Instance<Type> FindOrCreate(const Asset<AssetData>& asset);

            //! Calls FindOrCreate using a random InstanceId
            Data::Instance<Type> Create(const Asset<AssetData>& asset);

        private:
            InstanceDatabase(const AssetType& assetType);
            ~InstanceDatabase();

            static const char* GetEnvironmentName();

            // Utility function called by InstanceData to remove the instance from the database.
            void ReleaseInstance(InstanceData* instance, const InstanceId& instanceId) override;

            void ValidateSameAsset(InstanceData* instance, const Data::Asset<AssetData>& asset) const;

            // Performs a thread-safe search for the InstanceHandler for a given asset type.
            bool FindHandler(const AssetType& assetType, InstanceHandler<Type>& handlerOut);

            mutable AZStd::shared_mutex m_handlersMutex;
            AZStd::unordered_map<AssetType, InstanceHandler<Type>> m_handlers;

            mutable AZStd::shared_mutex m_databaseMutex;
            AZStd::unordered_map<InstanceId, Type*> m_database;

            // All instances created by this InstanceDatabase will be for assets derived from this type.
            AssetType m_baseAssetType;

            static EnvironmentVariable<InstanceDatabase*> ms_instance;
        };

        template <typename Type>
        EnvironmentVariable<InstanceDatabase<Type>*> InstanceDatabase<Type>::ms_instance = nullptr;

        template <typename Type>
        InstanceDatabase<Type>::~InstanceDatabase()
        {
#ifdef AZ_DEBUG_BUILD
            for (const auto& keyValue : m_database)
            {
                const InstanceId& instanceId = keyValue.first;
                const AZStd::string& stringValue = instanceId.ToString<AZStd::string>();
                AZ_Printf("InstanceDatabase", "\tLeaked Instance: %s\n", stringValue.c_str());
            }
#endif

            AZ_Error(
                "InstanceDatabase", m_database.empty(),
                "AZ::Data::%s still has active references.", Type::GetDatabaseName());
        }

        template <typename Type>
        void InstanceDatabase<Type>::AddHandler(const AssetType& assetType, const InstanceHandler<Type>& handler)
        {
            AZ_Assert(handler.m_createFunction, "You are required to provide a create function to InstanceDatabase.");

            AZStd::unique_lock<AZStd::shared_mutex> lock(m_handlersMutex);
            auto result = m_handlers.emplace(assetType, handler);
            AZ_Assert(result.second, "An InstanceHandler already exists for this AssetType");
        }

        template <typename Type>
        void InstanceDatabase<Type>::AddHandler(const AssetType& assetType, typename InstanceHandler<Type>::CreateFunction createFunction)
        {
            InstanceHandler<Type> instanceHandler;
            instanceHandler.m_createFunction = createFunction;

            AddHandler(assetType, instanceHandler);
        }

        template <typename Type>
        void InstanceDatabase<Type>::RemoveHandler(const AssetType& assetType)
        {
            AZStd::unique_lock<AZStd::shared_mutex> lock(m_handlersMutex);
            m_handlers.erase(assetType);
        }

        template <typename Type>
        bool InstanceDatabase<Type>::FindHandler(const AssetType& assetType, InstanceHandler<Type>& handlerOut)
        {
            AZStd::shared_lock<AZStd::shared_mutex> lock(m_handlersMutex);

            auto handlerIter = m_handlers.find(assetType);
            if (handlerIter != m_handlers.end())
            {
                // Since the handler is just a couple pointers, we copy the handler so we can
                // release the lock right away.
                handlerOut = handlerIter->second;
                return true;
            }
            else
            {
                return false;
            }
        }

        template <typename Type>
        Data::Instance<Type> InstanceDatabase<Type>::Find(const InstanceId& id) const
        {
            AZStd::shared_lock<AZStd::shared_mutex> lock(m_databaseMutex);
            auto iter = m_database.find(id);
            if (iter != m_database.end())
            {
                return iter->second;
            }
            return nullptr;
        }

        template <typename Type>
        Data::Instance<Type> InstanceDatabase<Type>::FindOrCreate(const InstanceId& id, const Asset<AssetData>& asset)
        {
            if (!id.IsValid())
            {
                return nullptr;
            }

            // Try to find the entry using a shared lock, which will be faster if the instance already exists.
            {
                AZStd::shared_lock<AZStd::shared_mutex> lock(m_databaseMutex);
                auto iter = m_database.find(id);
                if (iter != m_database.end())
                {
                    InstanceData* data = static_cast<InstanceData*>(iter->second);
                    ValidateSameAsset(data, asset);
                    
                    return iter->second;
                }
            }

            // Take a reference so we can mutate it.
            Data::Asset<Data::AssetData> assetLocal = asset;

            if (!assetLocal.IsReady())
            {
                assetLocal = AssetManager::Instance().GetAsset(
                    assetLocal.GetId(), assetLocal.GetType(),
                    true,       // queueLoadData
                    nullptr,    // assetLoadFilterCB
                    true);      // loadBlocking

                // Failed to load the asset
                if (!assetLocal.IsReady())
                {
                    return nullptr;
                }
            }

            if (!azrtti_istypeof(m_baseAssetType, assetLocal.Get()))
            {
                InstanceHandler<Type> instanceHandler;

                // If a handler was incorrectly registered for an unrelated asset type, this is the 
                // first chance we have to discover that fact, because up until now all we had was two
                // TypeIds.
                if (FindHandler(assetLocal.GetType(), instanceHandler))
                {
                    AZ_Assert(false, "An InstanceHandler was added for asset type %s which is not a subclass of the base asset type %s.",
                        assetLocal.GetType().ToString<AZStd::string>().data(),
                        m_baseAssetType.ToString<AZStd::string>().data()
                    );

                    return nullptr;
                }
            }

            // Take a full lock for insertion.
            AZStd::unique_lock<AZStd::shared_mutex> lock(m_databaseMutex);

            // Search again in case someone else got here first.
            auto iter = m_database.find(id);
            if (iter != m_database.end())
            {
                InstanceData* data = static_cast<InstanceData*>(iter->second);
                ValidateSameAsset(data, asset);

                return iter->second;
            }

            // Emplace a new instance and return it.
            InstanceHandler<Type> instanceHandler;
            if (FindHandler(assetLocal.GetType(), instanceHandler))
            {
                Data::Instance<Type> instance = instanceHandler.m_createFunction(assetLocal.Get());
                if (instance)
                {
                    instance->m_id = id;
                    instance->m_parentDatabase = this;
                    instance->m_assetId = assetLocal.GetId();
                    instance->m_assetType = assetLocal.GetType();
                    m_database.emplace(id, instance.get());
                }
                return AZStd::move(instance);
            }
            else
            {
                AZ_Warning(
                    "InstanceDatabase", false,
                    "No InstanceHandler found for asset type %s", assetLocal.GetType().ToString<AZStd::string>().data());
                return nullptr;
            }
        }

        template <typename Type>
        Data::Instance<Type> InstanceDatabase<Type>::FindOrCreate(const Asset<AssetData>& asset)
        {
            return FindOrCreate(Data::InstanceId::CreateFromAssetId(asset.GetId()), asset);
        }

        template <typename Type>
        Data::Instance<Type> InstanceDatabase<Type>::Create(const Asset<AssetData>& asset)
        {
            return FindOrCreate(Data::InstanceId::CreateRandom(), asset);
        }

        template <typename Type>
        void InstanceDatabase<Type>::ReleaseInstance(InstanceData* instance, const InstanceId& instanceId)
        {
            AZStd::unique_lock<AZStd::shared_mutex> lock(m_databaseMutex);
            
            // If instanceId doesn't exist in m_database that means the instance was already deleted on another thread.
            // We check and make sure the pointers match before erasing, just in case some other InstanceData was created with the same ID.
            // We re-check the m_useCount in case some other thread requested an instance from the database after we decremented m_useCount.
            // We change m_useCount to -1 to be sure another thread doesn't try to clean up the instance (though the other checks probably cover that).
            auto instanceItr = m_database.find(instanceId);
            int32_t expectedRefCount = 0;
            if (instanceItr != m_database.end() &&
                instanceItr->second == instance &&
                instance->m_useCount.compare_exchange_strong(expectedRefCount, -1))
            {
                m_database.erase(instance->GetId());

                InstanceHandler<Type> instanceHandler;
                if (FindHandler(instance->GetAssetType(), instanceHandler))
                {
                    instanceHandler.m_deleteFunction(static_cast<Type*>(instance));
                }
                else
                {
                    AZ_Assert(false,
                        "Cannot delete Instance. No InstanceHandler found for asset type %s", instance->GetAssetType().ToString<AZStd::string>().data());
                }
            }
        }

        template <typename Type>
        void InstanceDatabase<Type>::ValidateSameAsset(InstanceData* instance, const Data::Asset<AssetData>& asset) const
        {
            /**
             * The following validation layer is disabled in release, but is designed to catch a couple related edge cases
             * that might result in difficult to track bugs.
             *  - The user provides an id that collides with a different id.
             *  - The user attempts to provide a different asset when requesting the same instance id.
             *
             * In either case, the probable result is that an instance is returned that does not match the asset id provided
             * by the caller, which is not valid and probably not what the user expected. The validation layer will throw an
             * error to alert them.
             */

        #if defined (AZ_DEBUG_BUILD)
            AZ_Error("InstanceDatabase", instance->m_assetId == asset.GetId(),
                "InstanceDatabase::FindOrCreate found the requested instance, but a different asset was used to create it. "
                "Instances of a specific id should be acquired using the same asset. Either make sure the instance id "
                "is actually unique, or that you are using the same asset each time for that particular id.");
        #else
            AZ_UNUSED(instance);
            AZ_UNUSED(asset);
        #endif
        }

        template <typename Type>
        InstanceDatabase<Type>::InstanceDatabase(const AssetType& assetType) 
            : m_baseAssetType(assetType)
        {
        }

        template <typename Type>
        void InstanceDatabase<Type>::Create(const AssetType& assetType)
        {
            AZ_Assert(!ms_instance || !ms_instance.Get(), "InstanceDatabase already created!");

            if (!ms_instance)
            {
                ms_instance = Environment::CreateVariable<InstanceDatabase*>(GetEnvironmentName());
            }

            if (!ms_instance.Get())
            {
                ms_instance.Set(aznew InstanceDatabase<Type>(assetType));
            }
        }

        template <typename Type>
        void InstanceDatabase<Type>::Create(const AssetType& assetType, const InstanceHandler<Type>& handler)
        {
            Create(assetType);
            Instance().AddHandler(assetType, handler);
        }

        template <typename Type>
        void InstanceDatabase<Type>::Destroy()
        {
            AZ_Assert(ms_instance, "InstanceDatabase not created!");
            delete (*ms_instance);
            *ms_instance = nullptr;
        }

        template <typename Type>
        bool InstanceDatabase<Type>::IsReady()
        {
            if (!ms_instance)
            {
                ms_instance = Environment::FindVariable<InstanceDatabase*>(GetEnvironmentName());
            }

            return ms_instance && *ms_instance;
        }

        template <typename Type>
        InstanceDatabase<Type>& InstanceDatabase<Type>::Instance()
        {
            if (!ms_instance)
            {
                ms_instance = Environment::FindVariable<InstanceDatabase*>(GetEnvironmentName());
            }

            AZ_Assert(ms_instance && *ms_instance, "InstanceDatabase<%s> has not been initialized yet.", AzTypeInfo<Type>::Name());
            return *(*ms_instance);
        }

        template <typename Type>
        const char* InstanceDatabase<Type>::GetEnvironmentName()
        {
            static_assert(HasInstanceDatabaseName<Type>::value, "All classes used as instances in an InstanceDatabase need to define AZ_INSTANCE_DATA in the class.");
            return Type::GetDatabaseName();
        }
    }
}