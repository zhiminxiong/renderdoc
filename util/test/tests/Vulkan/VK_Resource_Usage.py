import renderdoc as rd
import rdtest

class VK_Resource_Usage(rdtest.TestCase):
    demos_test_name = 'VK_Resource_Usage'
    resourceUsages = {}
    eids = []

    def add_action(self, action: rd.ActionDescription):
        self.eids.append(action.eventId)
        for c in action.children:
            self.add_action(c)
        for e in action.events:
            self.eids.append(e.eventId)

    def check_resource_usage(self, res: rd.ResourceDescription, expectedUsages=[]):
        usages = self.resourceUsages[res.resourceId]
        if len(usages) != len(expectedUsages):
            for u in usages:
                rdtest.log.print(f"Resource '{res.name}' {res.resourceId} usage EID:{u.eventId} usage:{u.usage.name}")
            raise rdtest.TestFailureException(f"'{res.name}' {res.resourceId} Incorrect resource usages count expected:{len(expectedUsages)} actual:{len(usages)}")
        for i, u in enumerate(usages):
            eid, usage = expectedUsages[i]
            if u.usage != usage:
                raise rdtest.TestFailureException(f"'{res.name}' {res.resourceId} EID:{u.eventId} Incorrect resource usage expected:{usage.name} actual:{u.usage.name}")
            if u.eventId != eid:
                raise rdtest.TestFailureException(f"'{res.name}' {res.resourceId} usage:{u.usage.name} Incorrect resource usage EID expected:{eid} actual:{u.eventId}")

    def check_capture(self):
        # Cache the resource usage before running any replay i.e. without calling SetFrameEvent
        resources = self.controller.GetResources()
        for res in resources:
            self.resourceUsages[res.resourceId] = self.controller.GetUsage(res.resourceId)

        drawIndirectCount = self.find_action("Draw Indirect Count") is not None
        rdtest.log.print(f"Has Draw Indirect Count: {'Yes' if drawIndirectCount else 'No'}")

        nestedSecondaries = self.find_action("Nested Secondary Command Buffer") is not None
        rdtest.log.print(f"Has Nested Secondary Command Buffer: {'Yes' if nestedSecondaries else 'No'}")

        descBuffer = self.find_action("Descriptor Buffer") is not None
        rdtest.log.print(f"Has Descriptor Buffer: {'Yes' if descBuffer else 'No'}")

        countDrawIndirectCount = 30 if drawIndirectCount else 0
        countNested = 39 if nestedSecondaries else 0
        countDescBufferCopy = 10 if descBuffer else 0
        countDescBuffer = 21 if descBuffer else 0
        countDescBuffer += countDescBufferCopy

        action = self.find_action("Draw")
        self.controller.SetFrameEvent(action.eventId, False)
        swapImage = self.controller.GetPipelineState().GetOutputTargets()[0].resource

        with rdtest.log.auto_section("Checking Resource Usage"):
            for res in self.controller.GetResources():
                expectedUsage = []
                if res.type == rd.ResourceType.Device:
                    expectedUsage = [(0,rd.ResourceUsage.Unused)]
                elif res.type == rd.ResourceType.Queue:
                    expectedUsage = [(0,rd.ResourceUsage.Unused)]
                elif res.type == rd.ResourceType.Pool:
                    expectedUsage = [(0,rd.ResourceUsage.Unused)]
                elif res.type == rd.ResourceType.SwapchainImage:
                    # the swap chain image has usage, anything else does not
                    if res.resourceId == swapImage:
                        expectedUsage = [(6,rd.ResourceUsage.Barrier), 
                                        (6,rd.ResourceUsage.Discard), 
                                        (7,rd.ResourceUsage.Clear), 
                                        (8,rd.ResourceUsage.Barrier), 
                                        (32,rd.ResourceUsage.ColorTarget), 
                                        (35,rd.ResourceUsage.ColorTarget), 
                                        (42,rd.ResourceUsage.ColorTarget), 
                                        (45,rd.ResourceUsage.ColorTarget), 
                                        (59,rd.ResourceUsage.ColorTarget), 
                                        (62,rd.ResourceUsage.ColorTarget), 
                                        (73,rd.ResourceUsage.ColorTarget), 
                                        (76,rd.ResourceUsage.ColorTarget), 
                                        (119,rd.ResourceUsage.ColorTarget), 
                                        (123,rd.ResourceUsage.ColorTarget), 
                                        (124,rd.ResourceUsage.ColorTarget), 
                                        (125,rd.ResourceUsage.ColorTarget), 
                                        (126,rd.ResourceUsage.ColorTarget), 
                                        (131,rd.ResourceUsage.ColorTarget), 
                                        (132,rd.ResourceUsage.ColorTarget), 
                                        (133,rd.ResourceUsage.ColorTarget), 
                                        (169,rd.ResourceUsage.ColorTarget), 
                                        (173,rd.ResourceUsage.ColorTarget), 
                                        (174,rd.ResourceUsage.ColorTarget), 
                                        (175,rd.ResourceUsage.ColorTarget), 
                                        (176,rd.ResourceUsage.ColorTarget), 
                                        (181,rd.ResourceUsage.ColorTarget), 
                                        (182,rd.ResourceUsage.ColorTarget), 
                                        (183,rd.ResourceUsage.ColorTarget), 
                                        (200,rd.ResourceUsage.ColorTarget), 
                                        (204,rd.ResourceUsage.ColorTarget), 
                                        (205,rd.ResourceUsage.ColorTarget), 
                                        (206,rd.ResourceUsage.ColorTarget), 
                                        (207,rd.ResourceUsage.ColorTarget), 
                                        (212,rd.ResourceUsage.ColorTarget), 
                                        (213,rd.ResourceUsage.ColorTarget), 
                                        (214,rd.ResourceUsage.ColorTarget), 
                                        (219,rd.ResourceUsage.ColorTarget)] 
                        if drawIndirectCount:
                            expectedUsage += [
                                        (240,rd.ResourceUsage.ColorTarget), 
                                        (241,rd.ResourceUsage.ColorTarget), 
                                        (242,rd.ResourceUsage.ColorTarget), 
                                        (247,rd.ResourceUsage.ColorTarget), 
                                        (248,rd.ResourceUsage.ColorTarget)]
                        if nestedSecondaries:
                            expectedUsage += [
                                        (243+countDrawIndirectCount+countDescBufferCopy,rd.ResourceUsage.ColorTarget), 
                                        (246+countDrawIndirectCount+countDescBufferCopy,rd.ResourceUsage.ColorTarget)] 
                        if descBuffer:
                            expectedUsage += [
                                        (248+countDrawIndirectCount+countNested,rd.ResourceUsage.ColorTarget), 
                                        (251+countDrawIndirectCount+countNested,rd.ResourceUsage.ColorTarget)] 

                        expectedUsage += [(227+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.Barrier)]
                    else:
                        expectedUsage = []
                elif res.type == rd.ResourceType.RenderPass:
                    expectedUsage = [(0,rd.ResourceUsage.Unused)]
                elif res.type == rd.ResourceType.Sync:
                    expectedUsage = [(0,rd.ResourceUsage.Unused)]
                elif res.type == rd.ResourceType.View:
                    expectedUsage = [(0,rd.ResourceUsage.Unused)]
                elif res.type == rd.ResourceType.Memory:
                    expectedUsage = [(0,rd.ResourceUsage.Unused)]
                elif res.type == rd.ResourceType.ShaderBinding:
                    expectedUsage = [(0,rd.ResourceUsage.Unused)]
                elif res.type == rd.ResourceType.Shader:
                    expectedUsage = [(0,rd.ResourceUsage.Unused)]
                elif res.type == rd.ResourceType.PipelineState:
                    expectedUsage = [(0,rd.ResourceUsage.Unused)]
                elif res.type == rd.ResourceType.Buffer:
                    if (res.name == "Vertex Buffer"):
                        expectedUsage = [(32,rd.ResourceUsage.VertexBuffer), 
                                        (35,rd.ResourceUsage.VertexBuffer),
                                        (42,rd.ResourceUsage.VertexBuffer),
                                        (45,rd.ResourceUsage.VertexBuffer),
                                        (59,rd.ResourceUsage.VertexBuffer), 
                                        (62,rd.ResourceUsage.VertexBuffer), 
                                        (73,rd.ResourceUsage.VertexBuffer), 
                                        (76,rd.ResourceUsage.VertexBuffer), 
                                        (119,rd.ResourceUsage.VertexBuffer), 
                                        (123,rd.ResourceUsage.VertexBuffer), 
                                        (124,rd.ResourceUsage.VertexBuffer), 
                                        (125,rd.ResourceUsage.VertexBuffer), 
                                        (126,rd.ResourceUsage.VertexBuffer), 
                                        (131,rd.ResourceUsage.VertexBuffer), 
                                        (132,rd.ResourceUsage.VertexBuffer), 
                                        (133,rd.ResourceUsage.VertexBuffer), 
                                        (169,rd.ResourceUsage.VertexBuffer), 
                                        (173,rd.ResourceUsage.VertexBuffer), 
                                        (174,rd.ResourceUsage.VertexBuffer), 
                                        (175,rd.ResourceUsage.VertexBuffer), 
                                        (176,rd.ResourceUsage.VertexBuffer), 
                                        (181,rd.ResourceUsage.VertexBuffer), 
                                        (182,rd.ResourceUsage.VertexBuffer), 
                                        (183,rd.ResourceUsage.VertexBuffer), 
                                        (200,rd.ResourceUsage.VertexBuffer), 
                                        (204,rd.ResourceUsage.VertexBuffer), 
                                        (205,rd.ResourceUsage.VertexBuffer), 
                                        (206,rd.ResourceUsage.VertexBuffer), 
                                        (207,rd.ResourceUsage.VertexBuffer), 
                                        (212,rd.ResourceUsage.VertexBuffer), 
                                        (213,rd.ResourceUsage.VertexBuffer), 
                                        (214,rd.ResourceUsage.VertexBuffer),
                                        (219,rd.ResourceUsage.VertexBuffer)] 
                        if drawIndirectCount:
                            expectedUsage += [
                                        (240,rd.ResourceUsage.VertexBuffer), 
                                        (241,rd.ResourceUsage.VertexBuffer), 
                                        (242,rd.ResourceUsage.VertexBuffer), 
                                        (247,rd.ResourceUsage.VertexBuffer), 
                                        (248,rd.ResourceUsage.VertexBuffer)]
                        if nestedSecondaries:
                            expectedUsage += [
                                        (243+countDrawIndirectCount+countDescBufferCopy,rd.ResourceUsage.VertexBuffer), 
                                        (246+countDrawIndirectCount+countDescBufferCopy,rd.ResourceUsage.VertexBuffer)]
                        if descBuffer:
                            expectedUsage += [
                                        (248+countDrawIndirectCount+countNested,rd.ResourceUsage.VertexBuffer), 
                                        (251+countDrawIndirectCount+countNested,rd.ResourceUsage.VertexBuffer)]
                    if (res.name == "Index Buffer"):
                        expectedUsage = [(35,rd.ResourceUsage.IndexBuffer),
                                        (45,rd.ResourceUsage.IndexBuffer),
                                        (62,rd.ResourceUsage.IndexBuffer),
                                        (76,rd.ResourceUsage.IndexBuffer),
                                        (131,rd.ResourceUsage.IndexBuffer),
                                        (132,rd.ResourceUsage.IndexBuffer),
                                        (133,rd.ResourceUsage.IndexBuffer),
                                        (181,rd.ResourceUsage.IndexBuffer),
                                        (182,rd.ResourceUsage.IndexBuffer),
                                        (183,rd.ResourceUsage.IndexBuffer),
                                        (212,rd.ResourceUsage.IndexBuffer),
                                        (213,rd.ResourceUsage.IndexBuffer),
                                        (214,rd.ResourceUsage.IndexBuffer)]
                        if drawIndirectCount:
                            expectedUsage += [
                                        (247,rd.ResourceUsage.IndexBuffer),
                                        (248,rd.ResourceUsage.IndexBuffer)]
                        if nestedSecondaries:
                            expectedUsage += [
                                        (246+countDrawIndirectCount+countDescBufferCopy,rd.ResourceUsage.IndexBuffer)]
                        if descBuffer:
                            expectedUsage += [
                                        (251+countDrawIndirectCount+countNested,rd.ResourceUsage.IndexBuffer)]
                    if (res.name == "Compute Buffer In"):
                        expectedUsage += [(87,rd.ResourceUsage.CS_Constants),
                                        (94,rd.ResourceUsage.CS_Constants)]
                        if nestedSecondaries:
                            expectedUsage += [(259+countDrawIndirectCount+countDescBufferCopy,rd.ResourceUsage.CS_Constants)]
                        if descBuffer:
                            expectedUsage += [(256+countDrawIndirectCount+countNested,rd.ResourceUsage.CS_Constants)]
                    if (res.name == "Compute Buffer Out"):
                        expectedUsage += [(87,rd.ResourceUsage.CS_RWResource),
                                        (94,rd.ResourceUsage.CS_RWResource)]
                        if nestedSecondaries:
                            expectedUsage += [(259+countDrawIndirectCount+countDescBufferCopy,rd.ResourceUsage.CS_RWResource)]
                        if descBuffer:
                            expectedUsage += [(256+countDrawIndirectCount+countNested,rd.ResourceUsage.CS_RWResource)]
                    if (res.name == "Indirect Data"):
                        expectedUsage += [(14,rd.ResourceUsage.Barrier),
                                        (15,rd.ResourceUsage.Clear),
                                        (16,rd.ResourceUsage.Barrier),
                                        (20,rd.ResourceUsage.CS_RWResource),
                                        (21,rd.ResourceUsage.Barrier),
                                        (95,rd.ResourceUsage.Barrier),
                                        (106,rd.ResourceUsage.CS_RWResource),
                                        (106,rd.ResourceUsage.Indirect),
                                        (107,rd.ResourceUsage.Barrier),
                                        (119,rd.ResourceUsage.Indirect),
                                        (122,rd.ResourceUsage.Indirect),
                                        (130,rd.ResourceUsage.Indirect),
                                        (136,rd.ResourceUsage.Barrier),
                                        (141,rd.ResourceUsage.Barrier),
                                        (142,rd.ResourceUsage.Clear),
                                        (143,rd.ResourceUsage.Barrier),
                                        (147,rd.ResourceUsage.CS_RWResource),
                                        (149,rd.ResourceUsage.Barrier),
                                        (151,rd.ResourceUsage.CS_RWResource),
                                        (151,rd.ResourceUsage.Indirect),
                                        (152,rd.ResourceUsage.CS_RWResource),
                                        (152,rd.ResourceUsage.Indirect),
                                        (153,rd.ResourceUsage.Barrier),
                                        (154,rd.ResourceUsage.CS_RWResource),
                                        (154,rd.ResourceUsage.Indirect),
                                        (155,rd.ResourceUsage.Barrier),
                                        (169,rd.ResourceUsage.Indirect),
                                        (172,rd.ResourceUsage.Indirect),
                                        (180,rd.ResourceUsage.Indirect),
                                        (200,rd.ResourceUsage.Indirect),
                                        (203,rd.ResourceUsage.Indirect),
                                        (211,rd.ResourceUsage.Indirect),
                                        (219,rd.ResourceUsage.IndexBuffer),
                                        (219,rd.ResourceUsage.Indirect)]
                        if drawIndirectCount:
                            expectedUsage += [
                                        (224,rd.ResourceUsage.Indirect),
                                        (224,rd.ResourceUsage.Indirect),
                                        (227,rd.ResourceUsage.Indirect),
                                        (227,rd.ResourceUsage.Indirect),
                                        (231,rd.ResourceUsage.Indirect),
                                        (231,rd.ResourceUsage.Indirect),
                                        (235,rd.ResourceUsage.Indirect),
                                        (235,rd.ResourceUsage.Indirect),
                                        (239,rd.ResourceUsage.Indirect),
                                        (239,rd.ResourceUsage.Indirect),
                                        (246,rd.ResourceUsage.Indirect),
                                        (246,rd.ResourceUsage.Indirect)]
                        expectedUsage += [(223+countDrawIndirectCount,rd.ResourceUsage.Barrier)]
                        if nestedSecondaries:
                            expectedUsage += [
                                        (260+countDrawIndirectCount+countDescBufferCopy,rd.ResourceUsage.Barrier)]
                    if (res.name == "Barrier Buffer"):
                        expectedUsage = [(234+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.Barrier),
                                        (242+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.Barrier),
                                        (250+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.Barrier),
                                        (258+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.Barrier),
                                        (266+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.Barrier),
                                        (274+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.Barrier),
                                        (282+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.Barrier),
                                        (290+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.Barrier),
                                        (298+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.Barrier),
                                        (306+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.Barrier)]
                    if (res.name == "Barrier2 Buffer"):
                        expectedUsage = [(314+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.Barrier),
                                        (319+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.Barrier),
                                        (324+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.Barrier),
                                        (329+countDrawIndirectCount+countNested+countDescBuffer,rd.ResourceUsage.Barrier)]
                    if (res.name == "Descriptor Buffer"):
                        if descBuffer:
                            expectedUsage = [(227+countDrawIndirectCount,rd.ResourceUsage.Barrier), 
                                        (228+countDrawIndirectCount,rd.ResourceUsage.CopySrc),
                                        (229+countDrawIndirectCount,rd.ResourceUsage.Barrier),
                                        (230+countDrawIndirectCount,rd.ResourceUsage.Clear),
                                        (233+countDrawIndirectCount,rd.ResourceUsage.Barrier),
                                        (234+countDrawIndirectCount,rd.ResourceUsage.CopyDst)]
                    if (res.name == "Descriptor Backup Buffer"):
                        if descBuffer:
                            expectedUsage = [(227+countDrawIndirectCount,rd.ResourceUsage.Barrier), 
                                        (228+countDrawIndirectCount,rd.ResourceUsage.CopyDst),
                                        (233+countDrawIndirectCount,rd.ResourceUsage.Barrier),
                                        (234+countDrawIndirectCount,rd.ResourceUsage.CopySrc)]
                elif res.type == rd.ResourceType.Texture:
                    if (res.name == "Offscreen MSAA Image"):
                        expectedUsage = [(11,rd.ResourceUsage.Barrier), 
                                        (11,rd.ResourceUsage.Discard), 
                                        (12,rd.ResourceUsage.Clear)]
                    if (res.name == "Offscreen Image"):
                        expectedUsage = [(9,rd.ResourceUsage.Barrier), 
                                        (9,rd.ResourceUsage.Discard), 
                                        (10,rd.ResourceUsage.Clear), 
                                        (42,rd.ResourceUsage.PS_Resource), 
                                        (45,rd.ResourceUsage.PS_Resource), 
                                        (119,rd.ResourceUsage.PS_Resource), 
                                        (123,rd.ResourceUsage.PS_Resource), 
                                        (124,rd.ResourceUsage.PS_Resource), 
                                        (125,rd.ResourceUsage.PS_Resource), 
                                        (126,rd.ResourceUsage.PS_Resource), 
                                        (131,rd.ResourceUsage.PS_Resource), 
                                        (132,rd.ResourceUsage.PS_Resource), 
                                        (133,rd.ResourceUsage.PS_Resource), 
                                        (169,rd.ResourceUsage.PS_Resource), 
                                        (173,rd.ResourceUsage.PS_Resource), 
                                        (174,rd.ResourceUsage.PS_Resource), 
                                        (175,rd.ResourceUsage.PS_Resource), 
                                        (176,rd.ResourceUsage.PS_Resource), 
                                        (181,rd.ResourceUsage.PS_Resource), 
                                        (182,rd.ResourceUsage.PS_Resource), 
                                        (183,rd.ResourceUsage.PS_Resource), 
                                        (200,rd.ResourceUsage.PS_Resource), 
                                        (204,rd.ResourceUsage.PS_Resource), 
                                        (205,rd.ResourceUsage.PS_Resource), 
                                        (206,rd.ResourceUsage.PS_Resource), 
                                        (207,rd.ResourceUsage.PS_Resource), 
                                        (212,rd.ResourceUsage.PS_Resource), 
                                        (213,rd.ResourceUsage.PS_Resource), 
                                        (214,rd.ResourceUsage.PS_Resource),
                                        (219,rd.ResourceUsage.PS_Resource)]
                        if drawIndirectCount:
                            expectedUsage += [
                                        (240,rd.ResourceUsage.PS_Resource), 
                                        (241,rd.ResourceUsage.PS_Resource), 
                                        (242,rd.ResourceUsage.PS_Resource), 
                                        (247,rd.ResourceUsage.PS_Resource), 
                                        (248,rd.ResourceUsage.PS_Resource)]
                        if descBuffer:
                            expectedUsage += [
                                        (248+countDrawIndirectCount+countNested,rd.ResourceUsage.PS_Resource), 
                                        (251+countDrawIndirectCount+countNested,rd.ResourceUsage.PS_Resource)]
                elif res.type == rd.ResourceType.CommandBuffer:
                    expectedUsage = [(0,rd.ResourceUsage.Unused)]
                elif res.type == rd.ResourceType.DescriptorStore:
                    expectedUsage = [(0,rd.ResourceUsage.Unused)]
                elif res.type == rd.ResourceType.Sampler:
                    expectedUsage = [(0,rd.ResourceUsage.Unused)]
                else:
                    raise rdtest.TestFailureException(f"'{res.name}' {res.resourceId} Unexpected resource type {res.type.name}")
                rdtest.log.print(f"Resource '{res.name}' type:{res.type.name} {res.resourceId} usages:{len(self.controller.GetUsage(res.resourceId))} expectedUsages:{len(expectedUsage)}")
                self.check_resource_usage(res, expectedUsage)

        actions = self.controller.GetRootActions()
        for a in actions:
            self.add_action(a)

        # Select every event of the resource usage to ensure the EID is valid
        with rdtest.log.auto_section("Checking Resource Usage Events can be replayed"):
            for res in self.controller.GetResources():
                rdtest.log.print(f"Resource '{res.name}' type:{res.type.name} {res.resourceId}")
                usages = self.resourceUsages[res.resourceId]
                for u in usages:
                    eid = u.eventId
                    if eid == 0:
                        continue
                    self.controller.SetFrameEvent(eid, True)
                    if eid not in self.eids:
                        raise rdtest.TestFailureException(f"'{res.name}' {res.resourceId} Missing EID:{eid}")
        
