#pragma once

#include <SelectionComp.h>
#include <LambdaCreationFunctions.h>
#include "RedditComment.h"

namespace reddit {

class PositiveLabelSelection : public SelectionComp<Comment, Comment> {

 public:

  ENABLE_DEEP_COPY

  PositiveLabelSelection() = default;

  Lambda<bool> getSelection(Handle<Comment> in) override {
    return (makeLambdaFromMember(in, label) == makeLambda(in, [](Handle<Comment>& in){return 1;}));
  }

  Lambda<Handle<Comment>> getProjection(Handle<Comment> in) override {
    return makeLambda(in, [](Handle<Comment>& in) {
      Handle<Comment> tmp = makeObject<Comment>(*in);
      return tmp;
    });
  }
};

}
