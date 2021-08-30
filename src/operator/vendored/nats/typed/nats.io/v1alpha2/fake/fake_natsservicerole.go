// Code generated by client-gen. DO NOT EDIT.

package fake

import (
	"context"

	v1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	labels "k8s.io/apimachinery/pkg/labels"
	schema "k8s.io/apimachinery/pkg/runtime/schema"
	types "k8s.io/apimachinery/pkg/types"
	watch "k8s.io/apimachinery/pkg/watch"
	testing "k8s.io/client-go/testing"
	v1alpha2 "px.dev/pixie/src/operator/apis/nats.io/v1alpha2"
)

// FakeNatsServiceRoles implements NatsServiceRoleInterface
type FakeNatsServiceRoles struct {
	Fake *FakeNatsV1alpha2
	ns   string
}

var natsservicerolesResource = schema.GroupVersionResource{Group: "nats.io", Version: "v1alpha2", Resource: "natsserviceroles"}

var natsservicerolesKind = schema.GroupVersionKind{Group: "nats.io", Version: "v1alpha2", Kind: "NatsServiceRole"}

// Get takes name of the natsServiceRole, and returns the corresponding natsServiceRole object, and an error if there is any.
func (c *FakeNatsServiceRoles) Get(ctx context.Context, name string, options v1.GetOptions) (result *v1alpha2.NatsServiceRole, err error) {
	obj, err := c.Fake.
		Invokes(testing.NewGetAction(natsservicerolesResource, c.ns, name), &v1alpha2.NatsServiceRole{})

	if obj == nil {
		return nil, err
	}
	return obj.(*v1alpha2.NatsServiceRole), err
}

// List takes label and field selectors, and returns the list of NatsServiceRoles that match those selectors.
func (c *FakeNatsServiceRoles) List(ctx context.Context, opts v1.ListOptions) (result *v1alpha2.NatsServiceRoleList, err error) {
	obj, err := c.Fake.
		Invokes(testing.NewListAction(natsservicerolesResource, natsservicerolesKind, c.ns, opts), &v1alpha2.NatsServiceRoleList{})

	if obj == nil {
		return nil, err
	}

	label, _, _ := testing.ExtractFromListOptions(opts)
	if label == nil {
		label = labels.Everything()
	}
	list := &v1alpha2.NatsServiceRoleList{ListMeta: obj.(*v1alpha2.NatsServiceRoleList).ListMeta}
	for _, item := range obj.(*v1alpha2.NatsServiceRoleList).Items {
		if label.Matches(labels.Set(item.Labels)) {
			list.Items = append(list.Items, item)
		}
	}
	return list, err
}

// Watch returns a watch.Interface that watches the requested natsServiceRoles.
func (c *FakeNatsServiceRoles) Watch(ctx context.Context, opts v1.ListOptions) (watch.Interface, error) {
	return c.Fake.
		InvokesWatch(testing.NewWatchAction(natsservicerolesResource, c.ns, opts))

}

// Create takes the representation of a natsServiceRole and creates it.  Returns the server's representation of the natsServiceRole, and an error, if there is any.
func (c *FakeNatsServiceRoles) Create(ctx context.Context, natsServiceRole *v1alpha2.NatsServiceRole, opts v1.CreateOptions) (result *v1alpha2.NatsServiceRole, err error) {
	obj, err := c.Fake.
		Invokes(testing.NewCreateAction(natsservicerolesResource, c.ns, natsServiceRole), &v1alpha2.NatsServiceRole{})

	if obj == nil {
		return nil, err
	}
	return obj.(*v1alpha2.NatsServiceRole), err
}

// Update takes the representation of a natsServiceRole and updates it. Returns the server's representation of the natsServiceRole, and an error, if there is any.
func (c *FakeNatsServiceRoles) Update(ctx context.Context, natsServiceRole *v1alpha2.NatsServiceRole, opts v1.UpdateOptions) (result *v1alpha2.NatsServiceRole, err error) {
	obj, err := c.Fake.
		Invokes(testing.NewUpdateAction(natsservicerolesResource, c.ns, natsServiceRole), &v1alpha2.NatsServiceRole{})

	if obj == nil {
		return nil, err
	}
	return obj.(*v1alpha2.NatsServiceRole), err
}

// Delete takes name of the natsServiceRole and deletes it. Returns an error if one occurs.
func (c *FakeNatsServiceRoles) Delete(ctx context.Context, name string, opts v1.DeleteOptions) error {
	_, err := c.Fake.
		Invokes(testing.NewDeleteAction(natsservicerolesResource, c.ns, name), &v1alpha2.NatsServiceRole{})

	return err
}

// DeleteCollection deletes a collection of objects.
func (c *FakeNatsServiceRoles) DeleteCollection(ctx context.Context, opts v1.DeleteOptions, listOpts v1.ListOptions) error {
	action := testing.NewDeleteCollectionAction(natsservicerolesResource, c.ns, listOpts)

	_, err := c.Fake.Invokes(action, &v1alpha2.NatsServiceRoleList{})
	return err
}

// Patch applies the patch and returns the patched natsServiceRole.
func (c *FakeNatsServiceRoles) Patch(ctx context.Context, name string, pt types.PatchType, data []byte, opts v1.PatchOptions, subresources ...string) (result *v1alpha2.NatsServiceRole, err error) {
	obj, err := c.Fake.
		Invokes(testing.NewPatchSubresourceAction(natsservicerolesResource, c.ns, name, pt, data, subresources...), &v1alpha2.NatsServiceRole{})

	if obj == nil {
		return nil, err
	}
	return obj.(*v1alpha2.NatsServiceRole), err
}