/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_BUFFERCLIENT_H
#define MOZILLA_GFX_BUFFERCLIENT_H

#include "mozilla/StandardInteger.h"    // for uint64_t
#include <vector>                       // for vector
#include <map>                          // for map
#include "ipc/FenceUtils.h"
#include "mozilla/Assertions.h"         // for MOZ_CRASH
#include "mozilla/RefPtr.h"             // for TemporaryRef, RefCounted
#include "mozilla/gfx/Types.h"          // for SurfaceFormat
#include "mozilla/layers/CompositorTypes.h"
#include "mozilla/layers/TextureClient.h"  // for TextureClient, etc
#include "mozilla/layers/LayersTypes.h"  // for LayersBackend
#include "mozilla/layers/PCompositableChild.h"  // for PCompositableChild
#include "nsTraceRefcnt.h"              // for MOZ_COUNT_CTOR, etc
#include "gfxASurface.h"                // for gfxContentType

namespace mozilla {
namespace layers {

class CompositableClient;
class BufferTextureClient;
class ImageBridgeChild;
class CompositableForwarder;
class CompositableChild;
class SurfaceDescriptor;

/**
 * CompositableClient manages the texture-specific logic for composite layers,
 * independently of the layer. It is the content side of a CompositableClient/
 * CompositableHost pair.
 *
 * CompositableClient's purpose is to send texture data to the compositor side
 * along with any extra information about how the texture is to be composited.
 * Things like opacity or transformation belong to layer and not compositable.
 *
 * Since Compositables are independent of layers it is possible to create one,
 * connect it to the compositor side, and start sending images to it. This alone
 * is arguably not very useful, but it means that as long as a shadow layer can
 * do the proper magic to find a reference to the right CompositableHost on the
 * Compositor side, a Compositable client can be used outside of the main
 * shadow layer forwarder machinery that is used on the main thread.
 *
 * The first step is to create a Compositable client and call Connect().
 * Connect() creates the underlying IPDL actor (see CompositableChild) and the
 * corresponding CompositableHost on the other side.
 *
 * To do in-transaction texture transfer (the default), call
 * ShadowLayerForwarder::Attach(CompositableClient*, ShadowableLayer*). This
 * will let the LayerComposite on the compositor side know which CompositableHost
 * to use for compositing.
 *
 * To do async texture transfer (like async-video), the CompositableClient
 * should be created with a different CompositableForwarder (like
 * ImageBridgeChild) and attachment is done with
 * CompositableForwarder::AttachAsyncCompositable that takes an identifier
 * instead of a CompositableChild, since the CompositableClient is not managed
 * by this layer forwarder (the matching uses a global map on the compositor side,
 * see CompositableMap in ImageBridgeParent.cpp)
 *
 * Subclasses: Thebes layers use ContentClients, ImageLayers use ImageClients,
 * Canvas layers use CanvasClients (but ImageHosts). We have a different subclass
 * where we have a different way of interfacing with the textures - in terms of
 * drawing into the compositable and/or passing its contents to the compostior.
 */
class CompositableClient : public AtomicRefCounted<CompositableClient>
{
public:
  CompositableClient(CompositableForwarder* aForwarder);

  virtual ~CompositableClient();

  virtual TextureInfo GetTextureInfo() const = 0;

  LayersBackend GetCompositorBackendType() const;

  TemporaryRef<DeprecatedTextureClient>
  CreateDeprecatedTextureClient(DeprecatedTextureClientType aDeprecatedTextureClientType,
                                gfxContentType aContentType = GFX_CONTENT_SENTINEL);

  virtual TemporaryRef<BufferTextureClient>
  CreateBufferTextureClient(gfx::SurfaceFormat aFormat,
                            TextureFlags aFlags = TEXTURE_FLAGS_DEFAULT);

  // If we return a non-null TextureClient, then AsTextureClientDrawTarget will
  // always be non-null.
  TemporaryRef<TextureClient>
  CreateTextureClientForDrawing(gfx::SurfaceFormat aFormat,
                                TextureFlags aTextureFlags);

  virtual void SetDescriptorFromReply(TextureIdentifier aTextureId,
                                      const SurfaceDescriptor& aDescriptor)
  {
    MOZ_CRASH("If you want to call this, you should have implemented it");
  }

  /**
   * Establishes the connection with compositor side through IPDL
   */
  virtual bool Connect();

  void Destroy();

  CompositableChild* GetIPDLActor() const;

  // should only be called by a CompositableForwarder
  virtual void SetIPDLActor(CompositableChild* aChild);

  CompositableForwarder* GetForwarder() const
  {
    return mForwarder;
  }

  /**
   * This identifier is what lets us attach async compositables with a shadow
   * layer. It is not used if the compositable is used with the regular shadow
   * layer forwarder.
   *
   * If this returns zero, it means the compositable is not async (it is used
   * on the main thread).
   */
  uint64_t GetAsyncID() const;

  /**
   * Tells the Compositor to create a TextureHost for this TextureClient.
   */
  virtual bool AddTextureClient(TextureClient* aClient);

  /**
   * Tells the Compositor to delete the TextureHost corresponding to this
   * TextureClient.
   */
  virtual void RemoveTextureClient(TextureClient* aClient);

  // XXX: this function is only needed until Bug 897452 is fixed.
  virtual TemporaryRef<TextureClient> GetAddedTextureClient(uint64_t aTextureID);

  // XXX: this function is only needed until Bug 897452 is fixed.
  virtual TextureClientData* GetRemovingTextureClientData(uint64_t aTextureID);

  /**
   * A hook for the Compositable to execute whatever it held off for next transaction.
   */
  virtual void OnTransaction();

  /**
   * A hook for the when the Compositable is detached from it's layer.
   */
  virtual void OnDetach() {}

  /**
   * When texture deallocation must happen on the client side, we need to first
   * ensure that the compositor has already let go of the data in order
   * to safely deallocate it.
   *
   * This is implemented by registering a callback to postpone deallocation or
   * recycling of the shared data.
   *
   * This hook is called when the compositor notifies the client that it is not
   * holding any more references to the shared data so that this compositable
   * can run the corresponding callback.
   */
  void OnReplyTextureRemoved(uint64_t aTextureID);

  /**
   * Run all he registered callbacks (see the comment for OnReplyTextureRemoved).
   * Only call this if you know what you are doing.
   */
  void FlushTexturesToRemoveCallbacks();

  /**
   * Our IPDL actor is being destroyed, get rid of any shmem resources now.
   */
  virtual void OnActorDestroy() = 0;

  /**
   * If a fence is valid, wait until the fence is completed.
   * After the wait, clear it.
   */
  virtual void WaitAndResetReleaseFence() {}

  virtual void SetReleaseFence(FenceHandle aReleaseFenceHandle) {}

protected:
  // return the next texture ID
  uint64_t NextTextureID();

  struct TextureIDAndFlags {
    TextureIDAndFlags(uint64_t aID, TextureFlags aFlags)
    : mID(aID), mFlags(aFlags) {}
    uint64_t mID;
    TextureFlags mFlags;
  };

  // XXX map is necessary on the code that Bug 897452 is not fixed.
  std::map<uint64_t, RefPtr<TextureClient>> mAddedTextures;
  // The textures to destroy in the next transaction;
  nsTArray<TextureIDAndFlags> mTexturesToRemove;
  std::map<uint64_t, TextureClientData*> mTexturesToRemoveCallbacks;
  uint64_t mNextTextureID;
  CompositableChild* mCompositableChild;
  CompositableForwarder* mForwarder;
};

/**
 * IPDL actor used by CompositableClient to match with its corresponding
 * CompositableHost on the compositor side.
 *
 * CompositableChild is owned by a CompositableClient.
 */
class CompositableChild : public PCompositableChild
{
public:
  CompositableChild()
  : mCompositableClient(nullptr), mID(0)
  {
    MOZ_COUNT_CTOR(CompositableChild);
  }
  ~CompositableChild()
  {
    MOZ_COUNT_DTOR(CompositableChild);
  }

  void Destroy();

  void SetClient(CompositableClient* aClient)
  {
    mCompositableClient = aClient;
  }

  CompositableClient* GetCompositableClient() const
  {
    return mCompositableClient;
  }

  virtual void ActorDestroy(ActorDestroyReason why) MOZ_OVERRIDE;

  void SetAsyncID(uint64_t aID) { mID = aID; }
  uint64_t GetAsyncID() const
  {
    return mID;
  }
private:
  CompositableClient* mCompositableClient;
  uint64_t mID;
};

} // namespace
} // namespace

#endif
