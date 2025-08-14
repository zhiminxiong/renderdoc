import rdtest

class D3D11_Workgroup_Zoo(rdtest.Workgroup_Zoo):
    demos_test_name = 'D3D11_Workgroup_Zoo'
    internal = False

    def check_support(self, **kwargs):
        # Only allow this if explicitly run
        if kwargs['test_include'] == self.demos_test_name:
            return True, ''
        return False, 'Disabled test'

